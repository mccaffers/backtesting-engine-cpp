// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/utilities/backtestLog.hpp"
#include "shared/reporting/elasticPublisher.hpp"
#include "shared/tradingDefinitions/config/configuration.hpp"
#include "run/reporting/tradingResults.hpp"

export module operations;

import std;            // replaces <chrono>, <print>, <sstream>, <vector>, <memory>,
                       // <string>, <exception>
import priceData;      // PriceData
import tradeManager;   // TradeManager
import runLoop;        // trading::runTicks, RiskLimits, RunStatus
import resultsSummary;  // ResultsSummary
import symbolScale;    // symbol_scale::get
import strategy;       // IStrategy
import randomStrategy; // RandomStrategy
import strategyErrors; // UnknownStrategyError
import elasticClient;  // ElasticClient

export class Operations {
public:
    static void run(const std::vector<PriceData>& ticks,
                    const tradingDefinitions::Configuration& config);
};

namespace {

// Adding a new strategy means adding one branch here; nothing else in
// Operations needs to know about the concrete type.
std::unique_ptr<IStrategy> selectStrategy(const tradingDefinitions::Configuration& config) {
    const auto& name = config.STRATEGY.TRADING_VARIABLES.STRATEGY;
    if (name == "RandomStrategy") {
        return std::make_unique<RandomStrategy>(config.STRATEGY);
    }
    throw UnknownStrategyError(name);
}

} // namespace

void Operations::run(const std::vector<PriceData>& ticks,
                     const tradingDefinitions::Configuration& config) {

    // Function-local (stack) start time: each worker thread times only its own
    // run. steady_clock is monotonic, the correct clock for elapsed durations.
    const auto runStart = std::chrono::steady_clock::now();

    std::println("Operations: new run starting RUN_ID={} strategy={}",
                 config.RUN_ID,
                 config.STRATEGY.TRADING_VARIABLES.STRATEGY);

    TradeManager tradeManager;
    auto strategy = selectStrategy(config);

    // The per-tick loop (exit review -> re-entry gate -> entry -> manage) lives
    // in trading::runTicks so it can be driven with a deterministic strategy and
    // an inspectable TradeManager under test. The run-level risk limits make a
    // breaching run stop early (fail fast) instead of burning ticks.
    // The loss floor is pip-denominated; scale it to points using the run's
    // primary (first) symbol. Single-symbol/single-asset-class runs are exact.
    const std::string primarySymbol =
        config.SYMBOLS.substr(0, config.SYMBOLS.find(','));
    const trading::RiskLimits riskLimits{
        .startingBalance = config.STARTING_BALANCE,
        .maxLossPercent  = config.MAX_LOSS_PERCENT,
        .maxOpenTrades   = config.MAX_OPEN_TRADES,
        .pointsPerPip    = symbol_scale::get(primarySymbol),
    };
    const trading::RunStatus status =
        trading::runTicks(tradeManager, *strategy, ticks,
                          config.STRATEGY.TRADING_VARIABLES, riskLimits);

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
        } else {
            std::println("Operations: run RUN_ID={} completed in {:.3f}s",
                         config.RUN_ID, durationSeconds);
        }
    }

    // Best-effort: persist this run's outcome to Elasticsearch — results for a
    // completed run, a failure doc for one cut off by the loss limit. The
    // backtest has already produced its summary, so nothing here may abort the
    // run. The put functions log every transport/HTTP outcome themselves (so
    // we do not re-log on a non-zero return, that would double the warning),
    // but a throw from JSON serialisation or the network layer bypasses their
    // logging, so the catch handlers emit the single warning for that path.
    try {
        if (status == trading::RunStatus::LossLimitBreached) {
            // Silencer for large sweeps: liquidated runs are expected noise
            // once the system is trusted, so the run config can opt out of
            // reporting them. Completed runs always report.
            if (!config.REPORT_FAILURES) {
                return;
            }
            // Open trades were liquidated at their last marked prices on the
            // breach, so calculatePnl() is the true account PnL at cutoff.
            // PnL is int64 points-per-lot; report it in pips.
            const double pnlPips = riskLimits.pointsPerPip != 0
                ? static_cast<double>(tradeManager.calculatePnl()) / riskLimits.pointsPerPip
                : 0.0;
            std::ostringstream reason;
            reason << "account loss limit reached: PnL " << pnlPips << " pips breached "
                   << config.MAX_LOSS_PERCENT << "% of starting balance "
                   << config.STARTING_BALANCE << " (open trades liquidated)";
            const TradingFailure failure{
                config.RUN_ID,
                TradingResults::nowIsoUtc(),
                durationSeconds,
                reason.str(),
                config,
                ResultsSummary::collect(tradeManager, config),
            };
            ElasticClient::putTradingFailure(failure);
        } else {
            const TradingResults results{
                config.RUN_ID,
                TradingResults::nowIsoUtc(),
                durationSeconds,
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
