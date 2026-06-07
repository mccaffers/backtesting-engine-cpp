// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "redisRunner.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>

#include "backtestLog.hpp"
#include "backtestRunner.hpp"
#include "jsonParser.hpp"
#include "queueKeys.hpp"
#include "redisConnection.hpp"
#include "threadPool.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace {

// One Redis command on the shared connection, returning a bulk string (or nil).
// A nil reply (RPOP/LINDEX out of range) maps to nullopt. The connection is NOT
// cancelled here, it stays open for the rest of the worker loop.
asio::awaitable<std::optional<std::string>> execOptionalString(
    std::shared_ptr<redis::connection> conn,
    redis::request req) {
    redis::response<std::optional<std::string>> resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);
    co_return std::move(std::get<0>(resp).value());
}

// One Redis command on the shared connection whose reply we ignore (e.g. LREM).
asio::awaitable<void> execIgnore(std::shared_ptr<redis::connection> conn,
                                 redis::request req) {
    redis::generic_response resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);
    co_return;
}

// Non-destructively reads the run that RPOP would take (the queue tail, i.e. the
// oldest run since LoadCommand LPUSHes onto the head). Multiple workers all
// observe the same run and pile onto it.
asio::awaitable<std::optional<std::string>> peekRunTail(std::shared_ptr<redis::connection> conn) {
    redis::request req;
    req.push("LINDEX", queue_keys::RUN, "-1");
    co_return co_await execOptionalString(conn, std::move(req));
}

// Claims one strategy off the run's per-RUN_ID list. nullopt once drained.
asio::awaitable<std::optional<std::string>> popStrategy(
    std::shared_ptr<redis::connection> conn,
    std::string strategyKey) {
    redis::request req;
    req.push("RPOP", strategyKey);
    co_return co_await execOptionalString(conn, std::move(req));
}

// Retires a run by removing its descriptor. Idempotent: LREM removes 0 if a peer
// worker already retired it.
asio::awaitable<void> removeRun(std::shared_ptr<redis::connection> conn,
                                std::string descriptorB64) {
    redis::request req;
    req.push("LREM", queue_keys::RUN, "0", descriptorB64);
    co_await execIgnore(conn, std::move(req));
}

// Drains BACKTESTING_QUEUE_RUN on a single long-lived connection. For each run it
// loads the QuestDB ticks once, drains that run's strategy list, then retires the
// run. When the queue is empty it waits and re-peeks rather than exiting, so the
// worker stays up as a daemon; only a Redis/DB/decode error leaves the loop
// (return 3).
asio::awaitable<int> drainRuns(std::shared_ptr<redis::connection> conn,
                               std::string questdbHost) {
    int exitCode = 0;

    try {
        // One pool for the whole worker, reused across every run. Sized to the
        // machine but capped at 6 backtests in flight as a starting point.
        const unsigned hw = std::thread::hardware_concurrency();
        ThreadPool pool(std::clamp(hw == 0 ? 6u : hw, 1u, 6u));

        // Loop forever, claiming one run per iteration. An empty queue makes us
        // wait and re-peek (below); only an exception leaves this loop.
        bool waitingLogged = false;
        for (;;) {
            const std::optional<std::string> descriptorB64 = co_await peekRunTail(conn);
            if (!descriptorB64.has_value()) {
                // Queue empty: stay alive and poll until work reappears. The timer
                // is co_awaited, so this suspends (not a busy wait) while keeping
                // the io_context and the Redis connection alive.
                if (!waitingLogged) {
                    std::cout << "RedisRunner: queue empty, waiting for work..."
                              << std::endl;
                    waitingLogged = true;
                }
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                continue;  // re-peek; never exit just because the queue is empty
            }
            waitingLogged = false;  // got a run; re-arm the idle log for next time

            const trading_definitions::RunConfiguration runCfg =
                JsonParser::parseRunConfigurationFromBase64(*descriptorB64);
            const std::string strategyKey =
                queue_keys::strategyKey(runCfg.RUN_ID);
            std::cout << "RedisRunner: picked up RUN_ID=" << runCfg.RUN_ID
                      << " SYMBOLS=" << runCfg.SYMBOLS
                      << " LAST_MONTHS=" << runCfg.LAST_MONTHS << std::endl;

            // Pull this run's tick data once, then reuse across every strategy.
            const std::vector<PriceData> ticks =
                loadTicks(questdbHost, runCfg.SYMBOLS, runCfg.LAST_MONTHS);

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
                    co_await popStrategy(conn, strategyKey);
                if (!strategyB64.has_value()) {
                    break;  // strategy list drained
                }

                // Reassemble the Configuration the rest of the pipeline expects.
                // Parsing stays on this thread; the worker only runs the backtest.
                trading_definitions::Configuration config{
                    runCfg.RUN_ID,
                    runCfg.SYMBOLS,
                    runCfg.LAST_MONTHS,
                    JsonParser::parseStrategyFromBase64(*strategyB64),
                };
                pool.submit([&ticks, cfg = std::move(config)]() {
                    runBacktestOnTicks(ticks, cfg);
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
            co_await removeRun(conn, *descriptorB64);

            std::cout << "RedisRunner: completed RUN_ID=" << runCfg.RUN_ID << " ("
                      << strategiesRun << " strateg"
                      << (strategiesRun == 1 ? "y" : "ies") << ")" << std::endl;
        }
    } catch (const std::exception& ex) {
        std::cerr << "RedisRunner aborted: " << ex.what() << std::endl;
        exitCode = 3;
    } catch (...) {
        std::cerr << "RedisRunner aborted: unknown error" << std::endl;
        exitCode = 3;
    }

    // Only reached when an exception aborted the drain — the loop waits on an
    // empty queue rather than exiting. Tear the connection down so
    // io_context::run() can return.
    conn->cancel();
    co_return exitCode;
}

}  // namespace

int RedisRunner::run(const std::string& questdbHost,
                     const std::string& redisHost,
                     int redisPort) {
    
    // Mute logs to prevent interleaved thread spam
    backtest_log::quiet = true;

    // Set up the async event loop
    asio::io_context ioc;
    
    // Establish Redis connection
    auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

    // State trackers for the coroutine's outcome
    int result = 0;
    std::exception_ptr error;

    // Launch the async task to process runs
    asio::co_spawn(
        ioc,
        drainRuns(conn, questdbHost),
        [&result, &error](std::exception_ptr e, int r) { // Completion callback
            if (e) {
                error = e; // Save exception if it failed
                return;
            }
            result = r;    // Save exit code if it succeeded
        });

    // Block the current thread and execute the async loop until finished
    ioc.run();

    // Unwrap and log any exceptions caught during the async execution
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& ex) {
            std::cerr << "RedisRunner failed: " << ex.what() << std::endl;
        }
        return 3; // Exit with error status
    }

    // Return the final execution status code
    return result;
}