// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/utilities/backtestLog.hpp"
#include "shared/utilities/env.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "load/redisLoader.hpp"
#include "run/reporting/elasticPublisher.hpp"
#include "shared/tradingDefinitions/config/configuration.hpp"
#include "run/reporting/tradingResults.hpp"

export module operations;

import std;            // replaces <chrono>, <print>, <span>, <sstream>, <vector>,
                       // <memory>, <string>, <exception>
import barStore;       // bars::BarStore — the run's shared bar pipeline
import entryConditions; // conditions::gateSeriesFor — the ATR gate's series
import rangeBarBuilder; // rangebar::RangeBarSpec — range-bar registrations
import priceData;      // PriceData
import tradeManager;   // TradeManager
import runLoop;        // trading::runTicks, RiskLimits, RunStatus
import resultsSummary;  // ResultsSummary
import symbolScale;    // symbol_scale::get
import strategy;       // IStrategy
import strategyFactory; // strategies::makeStrategy
import elasticClient;  // ElasticClient
import rollingWindow;  // rolling::nextWindow, rolling::nextRunConfiguration

export class Operations {
public:
    // chainWindows opts a run into the rolling-window ladder (see
    // rolling::nextWindow): true only on the Redis-queue path, so a direct
    // `run <host> <config>` invocation can never LPUSH new work into the
    // cluster's queue as a side effect. Ticks arrive as a span so a cached
    // superset can hand each window a sub-range without copying.
    static void run(std::span<const PriceData> ticks,
                    const tradingDefinitions::Configuration& config,
                    bool chainWindows = false);
};

namespace {

// Adapts one strategy to RedisLoader's chunked pull interface (built for
// whole sweeps): a single one-payload chunk, then exhausted.
class SinglePayloadSource final : public RedisLoader::ChunkSource {
public:
    explicit SinglePayloadSource(RedisLoader::KeyedPayload payload)
        : payload_(std::move(payload)) {}

    std::vector<RedisLoader::KeyedPayload> next() override {
        if (delivered_) {
            return {};
        }
        delivered_ = true;
        std::vector<RedisLoader::KeyedPayload> chunk;
        chunk.push_back(std::move(payload_));
        return chunk;
    }

private:
    RedisLoader::KeyedPayload payload_;
    bool delivered_ = false;
};

// Re-queues a finished strategy as a fresh single-strategy run over `next`.
// The StrategyConfig travels byte-identical (same UUID), so a strategy can be
// followed across its windows in Elasticsearch; only the run descriptor —
// fresh RUN_ID, next window — changes. Best-effort like the outcome puts: a
// Redis failure is logged and recorded, never allowed to abort the run's
// reporting (the chain link is simply lost).
void queueNextWindow(const tradingDefinitions::Configuration& config,
                     const rolling::Window next) {
    try {
        // One persistent Redis connection for the whole process, shared by
        // every pool thread. RedisLoader is single-threaded by design, so the
        // mutex serialises the pushes; a re-queue is two small pipelined
        // round-trips at most once per completed run, so contention is
        // negligible — not worth a connection per worker thread. Both statics
        // initialise lazily on first chain push, and a throwing constructor
        // is retried on the next call (magic statics), landing in this same
        // catch either way.
        static std::mutex loaderMutex;
        static RedisLoader loader(env::getOr("REDIS_HOST", "127.0.0.1"), 6379);
        const std::scoped_lock loaderLock(loaderMutex);

        const auto newRunId =
            boost::uuids::to_string(boost::uuids::random_generator()());

        // Same ordering contract as loadCommand: payload key first, then its
        // name on the run's strategy list, then the run advert — so a worker
        // can never see the run before its strategy is claimable.
        const nlohmann::json strategyJson = config.STRATEGY;
        SinglePayloadSource source(
            {queue_keys::strategyPayloadKey(newRunId, config.STRATEGY.UUID),
             strategyJson.dump()});
        if (loader.loadKeyedPayloadStream(queue_keys::strategyKey(newRunId),
                                          source,
                                          queue_keys::PAYLOAD_TTL_SECONDS) != 0) {
            throw std::runtime_error("strategy payload push failed");
        }

        // The advert goes on the rung's own chain queue (not RUN): workers
        // drain queue_keys::RUN_QUEUES in priority order, so grid sweeps and
        // earlier rungs always outrank this run.
        const nlohmann::json runJson =
            rolling::nextRunConfiguration(config, next, newRunId);
        if (loader.loadPayload(rolling::queueKeyFor(next), runJson.dump()) != 0) {
            throw std::runtime_error("run descriptor push failed");
        }

        if (!backtest_log::is_quiet()) {
            std::println("Operations: window ({},{}) complete — queued window ({},{}) as RUN_ID={}",
                         config.LAST_MONTHS, config.OFFSET_MONTHS,
                         next.lastMonths, next.offsetMonths, newRunId);
        }
    } catch (const std::exception& e) {
        backtest_log::error(
            std::string("Operations: rolling-window re-queue failed: ")
            + e.what());
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "Operations",
             std::string("rolling-window re-queue failed: ") + e.what(),
             config.RUN_ID});
    }
}

}  // namespace

void Operations::run(const std::span<const PriceData> ticks,
                     const tradingDefinitions::Configuration& config,
                     const bool chainWindows) {

    // Function-local (stack) start time: each worker thread times only its own
    // run. steady_clock is monotonic, the correct clock for elapsed durations.
    const auto runStart = std::chrono::steady_clock::now();

    // The per-tick loop (exit review -> re-entry gate -> entry -> manage) lives
    // in trading::runTicks so it can be driven with a deterministic strategy and
    // an inspectable TradeManager under test. The run-level risk limits make a
    // breaching run stop early (fail fast) instead of burning ticks.
    // The loss floor is pip-denominated; scale it to points using the run's
    // primary (first) symbol. Single-symbol/single-asset-class runs are exact.
    const std::string primarySymbol = config.SYMBOLS.substr(0, config.SYMBOLS.find(','));

    // Multi-symbol caveat (documented, not fixed): PnL is summed in raw integer
    // points across ALL of the run's symbols, but the loss floor is scaled with
    // the PRIMARY symbol's points-per-pip only. Symbols sharing a scale are
    // fine — EURUSD and USDJPY are both 10 points/pip (their priceScale differs,
    // but that's already baked into the stored prices), as is every FX pair.
    // The skew appears when a run mixes asset classes (FX 10, indices/
    // commodities 100, metals 1000): a 1-pip move on XAUUSD (1000 points) would
    // count as 100 EURUSD pips in the equity check. Revisit before running
    // cross-asset-class sweeps.
    const trading::RiskLimits riskLimits{
        .startingBalance    = config.STARTING_BALANCE,
        .maxLossPercent     = config.MAX_LOSS_PERCENT,
        .maxOpenTrades      = config.MAX_OPEN_TRADES,
        .maxTradesPerMinute = config.MAX_TRADES_PER_MINUTE,
        .peakHoursOnly      = config.PEAK_HOURS_ONLY,
        .pointsPerPip       = symbol_scale::get(primarySymbol),
        // Performance gate (evaluated at the bottom of runTicks): a finished
        // run only counts as Completed — reporting to the results/winners index and, on
        // the queue path, chaining to its next window — when its performance
        // score clears 5 on more than 5 decisive trades. One lucky winner on
        // a quiet window is a sample, not a strategy; it lands as
        // Underperformed, recorded in the final-record index only.
        .minPerformanceScore = boost::decimal::decimal64_t{5},
        .minDecisiveTrades   = 5,
        .lastMonths          = config.LAST_MONTHS,
    };

    TradeManager tradeManager;
    // Slippage stress toggle: every entry this run opens pays this many
    // tenths of a pip against the trade (see tradeManager.openTrade). Rides
    // in the run config so the results doc records the stress it ran under.
    tradeManager.entrySlippageTenthPips = config.ENTRY_SLIPPAGE_TENTH_PIPS;
    auto strategy = strategies::makeStrategy(config.STRATEGY);

    // The run's shared bar pipeline: every strategy OHLC timeframe plus the
    // ATR entry gate's series, registered before the first tick. {0,0}
    // OHLC_VARIABLES entries are the documented "builds no bars" sentinel
    // (RandomStrategy) and register nothing. One store per run — per-symbol
    // state inside covers multi-symbol runs.
    bars::BarStore barStore;
    for (const auto& ohlcVars : config.STRATEGY.OHLC_VARIABLES) {
        if (ohlcVars.OHLC_MINUTES >= 1 && ohlcVars.OHLC_COUNT >= 1) {
            barStore.registerSeries(std::chrono::minutes{ohlcVars.OHLC_MINUTES},
                                    ohlcVars.OHLC_COUNT);
        }
    }
    // Range-bar series, same sentinel convention: an all-zeros (or partially
    // zero) RANGE_VARIABLES entry registers nothing, anything negative
    // already failed the config parse.
    for (const auto& rangeVars : config.STRATEGY.RANGE_VARIABLES) {
        if (rangeVars.RANGE_ATR_TICK_WINDOW >= 1 &&
            rangeVars.RANGE_ATR_PERCENT >= 1 && rangeVars.RANGE_COUNT >= 1) {
            barStore.registerRangeSeries(rangebar::RangeBarSpec{
                .atrTickWindow = rangeVars.RANGE_ATR_TICK_WINDOW,
                .atrPercent = rangeVars.RANGE_ATR_PERCENT,
                .count = rangeVars.RANGE_COUNT});
        }
    }
    const bars::SeriesSpec gateSeries =
        conditions::gateSeriesFor(config.STRATEGY);
    barStore.registerSeries(gateSeries.minutes, gateSeries.count);

    const trading::RunStatus status =
        trading::runTicks(tradeManager, *strategy, ticks,
                          config.STRATEGY.TRADING_VARIABLES, riskLimits,
                          &barStore, gateSeries);

    ResultsSummary::summarise(tradeManager, config);

    // Elapsed backtest time for this run, measured from the top of run(). The
    // Elasticsearch PUT below is deliberately excluded so the duration reflects
    // compute, not network latency.
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - runStart;
    const double durationSeconds = elapsed.count();

    // Per-run completion line, suppressed under concurrent (quiet) sweeps to
    // match the other per-run logs.
    if (!backtest_log::is_quiet()) {
        if (status == trading::RunStatus::LossLimitBreached) {
            std::println("Operations: run RUN_ID={} stopped after {:.3f}s — account loss limit reached",
                         config.RUN_ID, durationSeconds);
        } else if (status == trading::RunStatus::Underperformed) {
            std::println("Operations: run RUN_ID={} completed in {:.3f}s — below performance gate",
                         config.RUN_ID, durationSeconds);
        } else {
            std::println("Operations: run RUN_ID={} completed in {:.3f}s",
                         config.RUN_ID, durationSeconds);
        }
    }

    // Rolling-window chain (queue path only): a strategy that COMPLETES its
    // window advances to the next one on rolling::nextWindow's ladder — each
    // 3-month slice back through 9 months of history, then one final run over
    // the full 9 months, after which the ladder ends. Completed already embeds
    // the performance gate (score and decisive-trade thresholds on riskLimits
    // above): a liquidated run does not advance, and neither does one that
    // merely survived — the ladder exists to find live candidates, not to
    // spend three more windows on a config the winner selection would discard.
    // Off-ladder windows never chain, so a hand-queued one-off sweep stays
    // one-off.
    if (chainWindows && status == trading::RunStatus::Completed) {
        if (const auto next =
                rolling::nextWindow(config.LAST_MONTHS, config.OFFSET_MONTHS)) {
            queueNextWindow(config, *next);
        }
    }

    // Best-effort: queue this run's outcome for Elasticsearch — results for a
    // gate-cleared run, a failure doc for one cut off by the loss limit, and
    // only the terminal record for one that underperformed the gate. The
    // backtest has already produced its summary, so nothing here may abort the
    // run. The put functions serialise here, hand the documents to the
    // publisher's background flusher and return at once, so this worker is
    // free for its next backtest immediately; delivery, retries and
    // dead-lettering all happen on the flusher thread (which logs its own
    // failures — nothing to check here). A throw from JSON serialisation is
    // still this thread's, so the catch handlers report that path.
    try {
        // Every outcome doc carries the host that produced it, so a bad node in
        // a distributed sweep can be traced from any of the three indices.
        const std::string hostname = TradeFinal::localHostname();

        // Compact terminal record for this run, emitted for every run regardless
        // of outcome or the REPORT_FAILURES silencer below: the outcome flag
        // (success=1 means the performance gate was cleared, mirroring exactly
        // the runs the results/winners indices receive; underperformed and liquidated runs
        // are both success=0, told apart by the status keyword), how long it
        // took, and the host that produced it, alongside the run config. Sent
        // first so the silenced-failure early return below cannot skip it. For
        // an Underperformed run this is the ONLY record: the results/winners
        // indices only take gate-cleared runs (see below).
        const std::string statusLabel =
            status == trading::RunStatus::LossLimitBreached ? "loss_limit_breached"
            : status == trading::RunStatus::Underperformed  ? "underperformed"
                                                            : "completed";
        const TradeFinal tradeFinal{
            config.RUN_ID,
            TradingResults::nowIsoUtc(),
            durationSeconds,
            status == trading::RunStatus::Completed ? 1 : 0,
            statusLabel,
            hostname,
            config,
        };

        ElasticClient::putTradeFinal(tradeFinal);

        // Per-trade documents, opt-in via $ELASTIC_TRADES_ENABLED (default
        // OFF). TradeManager already accumulated every closed trade for the
        // stats summary, so the flag gates only this end-of-run serialisation
        // and enqueue — nothing on the per-tick path changes either way.
        // Queued before the REPORT_FAILURES silencer below so a breached run's
        // trades still land when its failure doc is suppressed; like the other
        // puts it sits outside the timed section and inside this best-effort
        // try/catch, so it cannot affect the backtest or its duration metric.
        if (env::getOr("ELASTIC_TRADES_ENABLED", "0") == "1") {
            ElasticClient::bulkPutTrades(tradeManager.getClosedTrades(),
                                         config, hostname);
        }

        if (status == trading::RunStatus::LossLimitBreached) {
            // Silencer for large sweeps: liquidated runs are expected noise once
            // the system is trusted, so the run config can opt out of the
            // detailed failure doc here. The terminal record above and completed
            // runs always report.
            if (!config.REPORT_FAILURES) {
                return;
            }
            // Open trades were liquidated at their last marked prices on the
            // breach, so calculatePnl() is the true account PnL at cutoff.
            // PnL is int64 points × trade size; normalise by both points-per-pip
            // and size so the reported figure is pips of price movement.
            const int tradingSize =
                std::max(1, config.STRATEGY.TRADING_VARIABLES.TRADING_SIZE);
            const double pnlDivisor =
                static_cast<double>(riskLimits.pointsPerPip) * tradingSize;
            const double breachPnlPips = pnlDivisor != 0.0
                ? static_cast<double>(tradeManager.calculatePnl()) / pnlDivisor
                : 0.0;
            // The budget that was breached, in pips: balance × percent/100 read
            // directly as a pip count (the engine has no pip-value model — see
            // the floor derivation in runLoop).
            const double lossFloorPips = static_cast<double>(
                config.STARTING_BALANCE * config.MAX_LOSS_PERCENT / 100);
            // `reason` stays static so reason.keyword aggregates to one value
            // per failure class; the run-specific numbers go in the dedicated
            // numeric fields.
            const TradingFailure failure{
                .RUN_ID = config.RUN_ID,
                .timestamp = TradingResults::nowIsoUtc(),
                .durationSeconds = durationSeconds,
                .reason = "account loss limit reached (open trades liquidated)",
                .breachPnlPips = breachPnlPips,
                .lossFloorPips = lossFloorPips,
                .hostname = hostname,
                .config = config,
                .results = ResultsSummary::collect(tradeManager, config),
            };
            ElasticClient::putTradingFailure(failure);
        } else if (status == trading::RunStatus::Completed) {
            // The results/winners indices take only gate-cleared runs: they
            // land there, so a document's presence means "reported AND chained
            // to its next window" — exactly the population liveWinners selects
            // from. An Underperformed run writes nothing beyond the terminal
            // record above (and the opt-in per-trade docs): its stats are the
            // noise the gate exists to keep out of the results.
            const TradingResults results{
                config.RUN_ID,
                TradingResults::nowIsoUtc(),
                durationSeconds,
                hostname,
                config,
                ResultsSummary::collect(tradeManager, config),
            };
            ElasticClient::putTradingResults(results);
        }
    } catch (const std::exception& e) {
        backtest_log::error(std::string("Operations: outcome put failed: ")
                            + e.what());
        // Best-effort: also record the failure in Elasticsearch (noexcept).
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "Operations", e.what(), config.RUN_ID});
    } catch (...) {
        backtest_log::error("Operations: outcome put failed: unknown error");
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "Operations", "unknown error", config.RUN_ID});
    }
}
