// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/consumer/drainRuns.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>

#include "shared/utilities/jsonParser.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "shared/utilities/threadPool.hpp"
#include "shared/reporting/elasticPublisher.hpp"
#include "shared/redis/consumer/runQueue.hpp"
#include "run/execution/runnerBridge.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace redis_runner {

asio::awaitable<int> drainRuns(std::shared_ptr<redis::connection> conn,
                               std::string questdbHost) {
    int exitCode = 0;

    try {

        // Get CPU threads available, max of 6
        const unsigned hw = std::thread::hardware_concurrency();
        ThreadPool pool(std::clamp(hw != 0 ? hw : 6u, 1u, 6u));

        // Loop forever, claiming one run per iteration. An empty queue makes us
        // wait and re-peek (below); only an exception blows the loop
        bool waitingLogged = false;
        for (;;) {
            const std::optional<std::string> descriptorB64 = co_await run_queue::peekRunTail(conn);
            if (!descriptorB64.has_value()) {
                // Queue empty: stay alive and poll until work reappears. The timer
                // is co_awaited, so this suspends (not a busy wait) while keeping
                // the io_context and the Redis connection alive.
                if (!waitingLogged) {
                    std::println("RedisRunner: queue empty, waiting for work...");
                    waitingLogged = true;
                }
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                continue;  // re-peek; never exit just because the queue is empty
            }
            waitingLogged = false;  // got a run; re-arm the idle log for next time

            const tradingDefinitions::RunConfiguration runCfg = JsonParser::parseRunConfigurationFromBase64(*descriptorB64);
            const std::string strategyKey = queue_keys::strategyKey(runCfg.RUN_ID);
            std::println("RedisRunner: picked up RUN_ID={} SYMBOLS={} LAST_MONTHS={}", runCfg.RUN_ID, runCfg.SYMBOLS, runCfg.LAST_MONTHS);

            // Pull this run's tick data once, then reuse across every strategy.
            // `ticks` is an opaque shared handle (see runnerBridge.hpp) so this
            // TU never names PriceData or imports a module.
            const std::shared_ptr<const LoadedTicks> ticks =
                bridgeLoadTicks(questdbHost, runCfg.SYMBOLS, runCfg.LAST_MONTHS);

            // Backtests run on the pool but every task reads this run's `ticks`
            // by reference (no copies). This guard joins all in-flight backtests
            // before `ticks` is destroyed, even if a co_await below throws.
            // wait() never throws, so it is safe during stack unwinding.
            struct Quiesce {
                ThreadPool& pool;
                ~Quiesce() { pool.wait(); }
            } quiesce{pool};

            // Drain the run's strategy list, competing with any other workers.
            // Redis stays on this coroutine thread; only the CPU-bound backtest
            // is handed to the pool. submit() applies backpressure, so we keep
            // popping at the rate the workers can absorb.
            int strategiesRun = 0;
            for (;;) {
                const std::optional<std::string> strategyB64 =
                    co_await run_queue::popStrategy(conn, strategyKey);
                if (!strategyB64.has_value()) {
                    break;  // strategy list drained
                }

                // Reassemble the Configuration the rest of the pipeline expects.
                // Parsing stays on this thread; the worker only runs the backtest.
                tradingDefinitions::Configuration config{
                    .RUN_ID = runCfg.RUN_ID,
                    .SYMBOLS = runCfg.SYMBOLS,
                    .LAST_MONTHS = runCfg.LAST_MONTHS,
                    .STARTING_BALANCE = runCfg.STARTING_BALANCE,
                    .MAX_LOSS_PERCENT = runCfg.MAX_LOSS_PERCENT,
                    .MAX_OPEN_TRADES = runCfg.MAX_OPEN_TRADES,
                    .REPORT_FAILURES = runCfg.REPORT_FAILURES,
                    .STRATEGY = JsonParser::parseStrategyFromBase64(*strategyB64),
                };
                pool.submit([&ticks, cfg = std::move(config)]() {
                    bridgeRunOnTicks(*ticks, cfg);
                });
                ++strategiesRun;
            }

            // Wait for this run's backtests to finish, then surface the first
            // failure (if any) so it aborts the drain exactly as the old
            // synchronous call did.
            pool.wait();
            if (std::exception_ptr err = pool.takeError()) {
                std::rethrow_exception(err);
            }

            // Retire the run. A failure here propagates and aborts the loop, we
            // never re-peek the same run and reload its ticks in a tight loop.
            co_await run_queue::removeRun(conn, *descriptorB64);

            std::println("RedisRunner: completed RUN_ID={} ({} strateg{})",
                         runCfg.RUN_ID, strategiesRun,
                         strategiesRun == 1 ? "y" : "ies");
        }
    } catch (const std::exception& ex) {
        std::println(stderr, "RedisRunner aborted: {}", ex.what());
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "drainRuns", ex.what(), ""});
        exitCode = 3;
    } catch (...) {
        std::println(stderr, "RedisRunner aborted: unknown error");
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "drainRuns", "unknown error", ""});
        exitCode = 3;
    }

    // Only reached when an exception aborted the drain — the loop waits on an
    // empty queue rather than exiting. Tear the connection down so
    // io_context::run() can return.
    conn->cancel();
    co_return exitCode;
}

}  // namespace redis_runner
