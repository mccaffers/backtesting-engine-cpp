// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "operations.hpp"
#include <chrono>
#include <print>
#include <vector>
#include <memory>
#include <string>
#include <exception>
#include "backtestLog.hpp"
#include "tradeManager.hpp"
#include "runLoop.hpp"
#include "reporting.hpp"
#include "tradingResults.hpp"
#include "reporting/elasticClient.hpp"
#include "strategies/strategy.hpp"
#include "strategies/randomStrategy.hpp"
#include "strategies/strategyErrors.hpp"

namespace {

// Adding a new strategy means adding one branch here; nothing else in
// Operations needs to know about the concrete type.
std::unique_ptr<IStrategy> selectStrategy(const trading_definitions::Configuration& config) {
    const auto& name = config.STRATEGY.TRADING_VARIABLES.STRATEGY;
    if (name == "RandomStrategy") {
        return std::make_unique<RandomStrategy>(config.STRATEGY);
    }
    throw UnknownStrategyError(name);
}

} // namespace

void Operations::run(const std::vector<PriceData>& ticks,
                     const trading_definitions::Configuration& config) {

    // Function-local (stack) start time: each worker thread times only its own
    // run. steady_clock is monotonic, the correct clock for elapsed durations.
    const auto runStart = std::chrono::steady_clock::now();

    std::println("Operations: new run starting RUN_ID={} strategy={}",
                 config.RUN_ID,
                 config.STRATEGY.TRADING_VARIABLES.STRATEGY);

    auto tradeManager = std::make_unique<TradeManager>();
    auto strategy = selectStrategy(config);

    // The per-tick loop (exit review -> re-entry gate -> entry -> manage) lives
    // in trading::runTicks so it can be driven with a deterministic strategy and
    // an inspectable TradeManager under test. Behaviour here is unchanged.
    trading::runTicks(*tradeManager, *strategy, ticks,
                      config.STRATEGY.TRADING_VARIABLES);

    Reporting::summarise(*tradeManager);

    // Elapsed backtest time for this run, measured from the top of run(). The
    // Elasticsearch PUT below is deliberately excluded so the duration reflects
    // compute, not network latency.
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - runStart;
    const double durationSeconds = elapsed.count();

    // Per-run completion line, suppressed under concurrent (quiet) sweeps to
    // match the other per-run logs.
    if (!backtest_log::quiet) {
        std::println("Operations: run RUN_ID={} completed in {:.3f}s",
                     config.RUN_ID, durationSeconds);
    }

    // Best-effort: persist this run's results to Elasticsearch. The backtest
    // has already produced its summary, so nothing here may abort the run.
    // putTradingResults logs every transport/HTTP outcome itself (so we do not
    // re-log on a non-zero return, that would double the warning), but a throw
    // from JSON serialisation or the network layer bypasses its logging, so the
    // catch handlers emit the single warning for that path.
    try {
        const TradingResults results{
            config.RUN_ID,
            TradingResults::nowIsoUtc(),
            durationSeconds,
            config,
            Reporting::collect(*tradeManager),
        };
        ElasticClient::putTradingResults(results);
    } catch (const std::exception& e) {
        backtest_log::error(std::string("Operations: trading-results put failed: ")
                            + e.what());
    } catch (...) {
        backtest_log::error("Operations: trading-results put failed: unknown error");
    }
}
