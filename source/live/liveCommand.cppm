// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveCommand — the `live` subcommand, now a thin orchestrator over the
// live/ modules:
//
//   config/liveSettings       env + argv -> Settings
//   winners/liveWinners       backtesting-winners-current _search -> ranked
//                             Winners
//   winners/liveStrategyCache Winners -> runnable WorkerSpecs (via
//                             strategyFactory)
//   execution/liveStrategyRunner  one worker thread per strategy;
//                             decide -> caps -> gate -> order, during every tick
//   execution/redisTradeGate  entry gate: Redis lock per (UUID, direction)
//   execution/redisPositionCounter  broker position count (PL# store) for
//                             the MAX_OPEN_TRADES cap
//   execution/redisPositionFeed  broker positions (PL#/PO#) mirrored into
//                             each worker's TradeManager book; strategy
//                             closes flow back through the close sink
//   execution/brokerOrderSink the order handoff: OrderIntent ->
//                             orderRequest -> orderChannel -> igRequests
//                             (gated, retried IG REST calls). Without IG
//                             session credentials in the environment every
//                             placement fails and extends its trade lock —
//                             loud, safe, and effectively a dry run.
//   monitoring/liveReporter   once-a-minute stats + final summary
//
// Timestamped flushed stdout lines come from the shared backtestLog module
// (shared/utilities), the same logLine every other subcommand uses.
//
// Ticks arrive as UDP datagrams (the same wire format ingest consumes, on
// the live stream's port). The broker owns positions and exits — nothing is
// persisted to QuestDB and no TradeManager exit logic runs here. Reachable
// Elasticsearch with at least one qualifying run is REQUIRED (no winners
// means nothing to trade, exit 1); env knobs are documented in liveSettings.
//
// The GMF only #includes Asio-free headers (the receiver hides Asio behind a
// pimpl), so it is safe to `import std` here.

module;

#include "run/reporting/elasticPublisher.hpp"
#include "shared/net/udpReceiver.hpp"

export module liveCommand;

import std;
import priceData;          // PriceData
import tickPacket;         // tick_packet::decodeTick
import backtestLog;        // backtest_log::logLine
import liveSettings;       // live::Settings
import liveWinners;        // live::fetchWinners
import liveStrategyCache;  // live::StrategyCache
import liveStrategyRunner; // live::StrategyRunner
import redisTradeGate;     // live::RedisTradeGate
import redisPositionCounter; // live::RedisPositionCounter
import redisPositionFeed;  // live::RedisPositionFeed
import brokerOrderSink;    // live::BrokerOrderSink, BrokerSinks
import igRequests;         // ig::IGMarketCalls, dynamoAuthProvider
import liveReporter;       // live::LiveReporter
import liveTrace;          // live_trace::init/emit — live-traces documents
import marketDefinitions;  // live::kTradableSymbols — the winners fan-out
import strategyFactory;    // strategies::kActiveStrategies

export class LiveCommand {
public:
    static int run(int argc, const char* argv[]);
};

int LiveCommand::run(const int argc, const char* argv[]) {
    using backtest_log::logLine;

    const live::Settings settings = live::Settings::fromEnv(argc, argv);

    // Arm the trace channel before any thread exists (init writes its env/
    // hostname strings unsynchronised on purpose — see liveTrace). Only this
    // subcommand ever arms it, so every other path's emits stay no-ops.
    live_trace::init(settings.tradingEnv);

    // The winners are fetched once, at startup — restart to pick up newer
    // backtest results. Zero winners is fatal on purpose: with no refresh, an
    // idling zero-strategy process is a permanent silent no-op that *looks*
    // alive; failing fast surfaces the real problem (Elastic down, wrong
    // host, no qualifying runs) to the operator/supervisor.
    logLine("LiveCommand: fetching winners from backtesting-winners-current "
            "(performanceScore > {}, maxDrawdownPercent <= {}, "
            "calmarScore >= {}, top {} per symbol+strategy, one query per "
            "pair: {} strategies x {} symbols)",
            settings.minScore, settings.maxDrawdownPercent,
            settings.minCalmarScore, settings.topPerGroup,
            strategies::kActiveStrategies.size(),
            live::kTradableSymbols.size());

    const std::vector<live::Winner> winners = live::fetchWinners(
        settings.minScore, settings.maxDrawdownPercent,
        settings.minCalmarScore, settings.topPerGroup,
        strategies::kActiveStrategies, live::kTradableSymbols);

    if (winners.empty()) {
        logLine("LiveCommand: no winning strategies available — nothing to "
                "trade; exiting (check ELASTIC_HOST and backtest results)");
        live_trace::emit("startupAborted", {}, {{"reason", "noWinners"}});
        elastic::flushQueuedDocuments();
        return 1;
    }

    std::vector<live::WorkerSpec> specs = live::StrategyCache::build(winners);
    if (specs.empty()) {
        logLine("LiveCommand: every winner failed to instantiate; exiting");
        live_trace::emit("startupAborted", {}, {{"reason", "noSpecs"}});
        elastic::flushQueuedDocuments();
        return 1;
    }

    const std::size_t workerCount = specs.size();
    // Fail loud at startup, not per signal: with no session the channel
    // refuses every placement (each burns its 2-minute failure brake) — the
    // operator should know before the first order, not after. One probe
    // pull of Auth#<env> from DynamoDB (AWS creds from the environment).
    const ig::AuthProvider auth = ig::dynamoAuthProvider(settings.tradingEnv);
    const bool haveAuth = auth().has_value();
    if (!haveAuth) {
        logLine("LiveCommand: WARNING — could not pull IG session "
                "Auth#{} from DynamoDB ({} table); every order will fail "
                "and extend its trade lock (check AWS credentials in the "
                "environment and the login service)",
                settings.tradingEnv, "MarketDataLive");
    }
    live::BrokerSinks sinks = live::BrokerOrderSink::make(
        settings.redisHost, settings.redisPort, settings.lockTtl,
        ig::IGMarketCalls::makeLiveOpen(settings.redisHost,
                                        settings.redisPort, auth),
        ig::IGMarketCalls::makeLiveClose(settings.redisHost,
                                         settings.redisPort, auth));
    live::StrategyRunner runner(
        std::move(specs),
        live::RedisTradeGate::make(settings.redisHost, settings.redisPort,
                                   settings.lockTtl),
        std::move(sinks.order),
        live::RedisPositionCounter::make(settings.redisHost,
                                         settings.redisPort),
        live::RedisPositionFeed::make(settings.redisHost, settings.redisPort),
        std::move(sinks.close));
    runner.start();

    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> dropped{0};

    net::UdpReceiver receiver(
        settings.bindAddr, settings.bindPort,
        [&](std::span<const std::byte> bytes) {
            const auto tick = tick_packet::decodeTick(bytes);
            if (!tick) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            runner.onTick(*tick);
            received.fetch_add(1, std::memory_order_relaxed);
        });

    logLine("LiveCommand: starting; binding udp://{}:{} "
            "({} live strategies, lockTtl={}s, IG order channel [{}]{})",
            settings.bindAddr, settings.bindPort, workerCount,
            settings.lockTtl.count(), settings.tradingEnv,
            haveAuth ? "" : " WITHOUT a session");
    if (live_trace::enabled()) {
        live_trace::emit(
            "startup", {},
            {{"winners", winners.size()},
             {"workers", workerCount},
             {"haveAuth", haveAuth},
             {"bindAddr", settings.bindAddr},
             {"bindPort", static_cast<std::int64_t>(settings.bindPort)},
             {"lockTtlSeconds", settings.lockTtl.count()},
             {"minScore", settings.minScore},
             {"maxDrawdownPercent", settings.maxDrawdownPercent},
             {"minCalmarScore", settings.minCalmarScore},
             {"ohlcPrepopulate", settings.ohlcPrepopulate}});
    }
    // The book is now fed from PL#/PO# (one sync per worker per ~15s) and
    // strategy closes flow back out — but two operational caveats remain
    // worth a line at every startup.
    logLine("LiveCommand: note — each worker mirrors its broker positions "
            "from Redis (PL#/PO#, ~15s cadence); deals whose confirm never "
            "resolved have no dealId and cannot be strategy-closed until "
            "one appears, and WITHOUT the external position producer PO# "
            "entries expire ~5min after open, fading them from the book");
    // Bar warm-up mode, loud at startup either way: bar-based strategies
    // (OhlcBreakout) otherwise build their windows tick by tick, so a
    // daily-bar window would trade nothing for weeks. The seeding itself
    // lives in ohlcBuilder / rangeBarBuilder (shared with run) and fires at
    // each symbol's first tick; per-seed lines land on stderr as bars load.
    // One switch covers both bar types — sweeps that silence warm-up DB
    // traffic silence range bars too.
    if (settings.ohlcPrepopulate) {
        logLine("LiveCommand: OHLC/range prepopulate on (the default) — bar "
                "histories seed from QuestDB (QUESTDB_HOST/QUESTDB_PORT) at "
                "each symbol's first tick; a failed seed logs and falls "
                "back to a cold start");
    } else {
        logLine("LiveCommand: OHLC/range prepopulate OFF (OHLC_PREPOPULATE=0) "
                "— bar strategies warm up from the live stream alone (a "
                "big-bar window can take days/weeks before decide() fires); "
                "unset it, or set 1, to seed bar histories from QuestDB");
    }

    // Declared after the counters and runner it borrows, so it is destroyed
    // (and its thread joined) before they are.
    live::LiveReporter reporter(received, dropped, runner);
    reporter.start();

    const bool ok = receiver.run();  // binds the socket, then blocks until SIGINT/SIGTERM

    reporter.stop();
    runner.stop();  // workers drain their queues, then join

    if (!ok) {
        live_trace::emit("startupAborted", {}, {{"reason", "bindFailed"}});
        elastic::flushQueuedDocuments();
        return 1;  // bind failed (bad address / port in use) — logged
    }

    reporter.logFinalSummary();
    // Deliver the shutdown/stats tail now, deterministically, rather than
    // leaving it to the publisher's exit-time flush.
    elastic::flushQueuedDocuments();
    return 0;
}
