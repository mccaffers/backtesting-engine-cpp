// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/queue/drainRuns.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

#include "shared/ipc/engineControl.hpp"
#include "shared/utilities/jsonParser.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "shared/utilities/threadPool.hpp"
#include "run/reporting/elasticPublisher.hpp"
#include "run/queue/runQueue.hpp"
#include "run/execution/runnerBridge.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace redis_runner {

asio::awaitable<int> drainRuns(std::shared_ptr<redis::connection> conn,
                               std::string questdbHost) {
    int exitCode = 0;

    try {

        // Map the shared-memory control channel so a local monitor can watch the
        // in-flight count and request a graceful stop. Best-effort: if the
        // segment can't be created the engine still runs, just unmonitored.
        std::optional<ipc::EngineControlChannel> control;
        std::atomic<std::int32_t>* gauge = nullptr;
        try {
            control.emplace();
            // state() exposes the control block living inside the mapped
            // segment; `&...active_jobs` takes the address of its atomic
            // in-flight counter so the ThreadPool can tick it up/down where a
            // monitor process can see it. A pointer (not a reference) so it
            // can stay nullptr — pool skips the bookkeeping — when the
            // segment failed to map.
            gauge = &control->state().active_jobs;
        } catch (const std::exception& ex) {
            std::println(stderr, "RedisRunner: shm control unavailable: {}", ex.what());
        }
        // Reusable stop-check predicate: a lambda capturing `control` by
        // reference. True once a monitor has written a nonzero stop_signal
        // into the shared segment (the acquire load pairs with the writer's
        // release store); short-circuits to false when the segment never
        // mapped. Polled at loop boundaries below — a stop never interrupts a
        // backtest mid-flight, it just stops new work being claimed.
        const auto stopRequested = [&] {
            return control &&
                   control->state().stop_signal.load(std::memory_order_acquire) != 0;
        };
        bool stopping = false;

        // Size the pool to 80% of the available CPU threads so the box keeps
        // headroom for Redis/IO and the host stays responsive. At least one
        // worker always runs; there is no upper cap. hardware_concurrency()
        // returns 0 when it can't tell, in which case we fall back to 1.
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned threads = hw != 0 ? std::max(1u, hw * 4 / 5) : 1u;
        ThreadPool pool(threads, gauge);

        // Loop forever, claiming one run per iteration. An empty queue makes us
        // wait and re-peek (below); only an exception or a stop request blows the
        // loop. Backtests from consecutive runs PIPELINE on the pool (tasks hold
        // their tick buffer by value), so single-strategy chained runs no longer
        // drain the pool between claims.
        bool waitingLogged = false;
        for (;;) {
            // Stop requested between runs (or while idle on the empty-queue timer,
            // which `continue`s back here): submit nothing further and fall
            // through to the drain-and-pause below.
            if (stopRequested()) {
                stopping = true;
                break;
            }

            // Surface the first infrastructure-shaped task failure from any
            // still-pipelining backtest. Strategy-scoped errors are contained
            // inside the tasks themselves, so anything here aborts the drain
            // exactly as the old per-run check did — at worst one run later.
            if (std::exception_ptr err = pool.takeError()) {
                std::rethrow_exception(err);
            }
            const std::optional<run_queue::PeekedRun> peeked =
                co_await run_queue::peekRunTail(conn);
            if (!peeked.has_value()) {
                // Every run queue empty: stay alive and poll until work
                // reappears. The timer is co_awaited, so this suspends (not a
                // busy wait) while keeping the io_context and the Redis
                // connection alive.
                if (!waitingLogged) {
                    std::println("RedisRunner: run queues empty, waiting for work...");
                    waitingLogged = true;
                }

                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                continue;  // re-peek; never exit just because the queue is empty
            }
            waitingLogged = false;  // got a run; re-arm the idle log for next time

            // A run descriptor that cannot be parsed is a poison pill: without
            // this guard it would abort the worker AND stay at the tail of the
            // queue to kill the next worker too. removeRun LREMs by the raw
            // base64 value, so the descriptor can be retired without ever
            // parsing it.
            tradingDefinitions::RunConfiguration runCfg;
            bool descriptorOk = true;
            try {
                runCfg = JsonParser::parseRunConfigurationFromBase64(peeked->descriptorB64);
            } catch (const std::exception& ex) {
                // co_await is illegal inside a catch handler, so the removal
                // happens just below, outside the try/catch.
                std::println(stderr, "RedisRunner: unparseable run descriptor, retiring: {}",
                             ex.what());
                elastic::putEngineException(
                    {elastic::nowIsoUtc(), "drainRuns",
                     std::string("unparseable run descriptor retired: ") + ex.what(), ""});
                descriptorOk = false;
            }
            if (!descriptorOk) {
                co_await run_queue::removeRun(conn, peeked->queueKey,
                                              peeked->descriptorB64);
                continue;
            }
            const std::string strategyKey = queue_keys::strategyKey(runCfg.RUN_ID);

            std::println("drainRuns, RUN_ID={} SYMBOLS={} LAST_MONTHS={} OFFSET_MONTHS={} QUEUE={} STRATEGY_KEY={}",
                         runCfg.RUN_ID, runCfg.SYMBOLS, runCfg.LAST_MONTHS,
                         runCfg.OFFSET_MONTHS, peeked->queueKey, strategyKey);

            // Claim the first strategy BEFORE the expensive tick load: when
            // several workers converge on a nearly-drained run, the losers
            // would otherwise each pay a full QuestDB fetch only to find the
            // list already empty.
            std::optional<std::string> payloadKey =
                co_await run_queue::popStrategyKey(conn, strategyKey);
            if (!payloadKey.has_value()) {
                // Another worker drained this run; retire it and re-peek.
                // removeRun's LREM is idempotent across competing workers.
                co_await run_queue::removeRun(conn, peeked->queueKey,
                                              peeked->descriptorB64);
                std::println("RedisRunner: RUN_ID={} already drained, retiring",
                             runCfg.RUN_ID);
                continue;
            }

            // This run's tick window, usually a slice of a cached superset —
            // the rolling ladder's single-strategy runs all share one load.
            // `slice` is an opaque value handle (see runnerBridge.hpp) so this
            // TU never names PriceData or imports a module. The quiesce runs
            // ONLY when a real QuestDB load is coming (cache miss/expiry): it
            // joins every pipelined backtest still reading a buffer the cache
            // is about to evict, and surfaces any failure they raised. Cache
            // hits claim and submit without ever waiting.
            const TickSlice slice = bridgeGetTicks(
                questdbHost, runCfg.SYMBOLS, runCfg.LAST_MONTHS,
                runCfg.OFFSET_MONTHS,
                /*beforeLoad=*/[&pool] {
                    pool.wait();
                    if (std::exception_ptr err = pool.takeError()) {
                        std::rethrow_exception(err);
                    }
                });

            // Drain the run's strategy list, competing with any other workers.
            // The list carries payload KEY NAMES: RPOP hands each name to
            // exactly one worker, then GETDEL consumes the payload stored
            // under it (atomically, leaving nothing behind). Redis stays on
            // this coroutine thread; only the CPU-bound backtest is handed to
            // the pool. submit() applies backpressure, so we keep popping at
            // the rate the workers can absorb. The first key was claimed above
            // (before the tick load), so the loop consumes `payloadKey` first
            // and pops the next one at the bottom.
            int strategiesRun = 0;
            for (;;) {
                const std::optional<std::string> strategyB64 =
                    co_await run_queue::takeStrategyPayload(conn, *payloadKey);
                if (!strategyB64.has_value()) {
                    // Popped a name whose payload is gone — a peer that crashed
                    // between its RPOP and GETDEL can't cause this (the name
                    // went with it); this is the safety-net TTL having reaped a
                    // long-stale payload. Skip it; the run still drains.
                    std::println(stderr,
                                 "RedisRunner: payload {} missing (expired?), skipping",
                                 *payloadKey);
                } else {
                    // Reassemble the Configuration the rest of the pipeline
                    // expects. Parsing stays on this thread; the worker only
                    // runs the backtest. A payload that fails to parse or
                    // validate is a poison pill: report it and move on — one
                    // bad strategy must not abort the worker (its payload is
                    // already consumed, so it cannot recur).
                    try {
                        tradingDefinitions::Configuration config{
                            .RUN_ID = runCfg.RUN_ID,
                            .SYMBOLS = runCfg.SYMBOLS,
                            .BATCH = runCfg.BATCH,
                            .EXECUTION_TS = runCfg.EXECUTION_TS,
                            .LAST_MONTHS = runCfg.LAST_MONTHS,
                            .OFFSET_MONTHS = runCfg.OFFSET_MONTHS,
                            .STARTING_BALANCE = runCfg.STARTING_BALANCE,
                            .MAX_LOSS_PERCENT = runCfg.MAX_LOSS_PERCENT,
                            .MAX_OPEN_TRADES = runCfg.MAX_OPEN_TRADES,
                            .MAX_TRADES_PER_MINUTE = runCfg.MAX_TRADES_PER_MINUTE,
                            .REPORT_FAILURES = runCfg.REPORT_FAILURES,
                            .PEAK_HOURS_ONLY = runCfg.PEAK_HOURS_ONLY,
                            .ENTRY_SLIPPAGE_TENTH_PIPS =
                                runCfg.ENTRY_SLIPPAGE_TENTH_PIPS,
                            .STRATEGY = JsonParser::parseStrategyFromBase64(*strategyB64),
                        };
                        // Strategy-scoped failures inside the backtest (unknown
                        // strategy name, config the strategy rejects) are
                        // likewise contained per task: reported, and the drain
                        // continues. Non-std exceptions still reach the pool's
                        // error slot and abort at the next loop-top check —
                        // those are not strategy-shaped. `slice` is captured BY
                        // VALUE: its shared handle keeps the tick buffer alive
                        // for this task even after the cache moves on, which is
                        // what lets runs pipeline without a per-run barrier.
                        pool.submit([slice, cfg = std::move(config)]() {
                            try {
                                bridgeRunOnTicks(slice, cfg);
                            } catch (const std::exception& ex) {
                                std::println(stderr,
                                             "RedisRunner: backtest failed (RUN_ID={} strategy={}): {}",
                                             cfg.RUN_ID, cfg.STRATEGY.UUID, ex.what());
                                elastic::putEngineException(
                                    {elastic::nowIsoUtc(), "backtest",
                                     std::string("strategy ") + cfg.STRATEGY.UUID
                                         + " failed: " + ex.what(),
                                     cfg.RUN_ID});
                            }
                        });
                        ++strategiesRun;
                    } catch (const std::exception& ex) {
                        std::println(stderr,
                                     "RedisRunner: unparseable strategy payload {} (RUN_ID={}), skipping: {}",
                                     *payloadKey, runCfg.RUN_ID, ex.what());
                        elastic::putEngineException(
                            {elastic::nowIsoUtc(), "drainRuns",
                             std::string("unparseable strategy payload skipped: ")
                                 + ex.what(),
                             runCfg.RUN_ID});
                    }
                }

                // Stop requested mid-run: stop claiming new strategies. The
                // Quiesce guard / pool.wait() below still drains everything
                // already in flight, and the gauge falls to 0 as it does.
                if (stopRequested()) {
                    stopping = true;
                    break;
                }
                payloadKey = co_await run_queue::popStrategyKey(conn, strategyKey);
                if (!payloadKey.has_value()) {
                    break;  // strategy list drained
                }
            }

            // Stop requested: leave this run in Redis (we stopped mid-list) and
            // break out to pause — don't claim the next run. The drain-and-wait
            // happens once, below the loop.
            if (stopping) {
                break;
            }

            // Retire the run as soon as its strategy list is drained — its
            // backtests may still be pipelining on the pool, but every payload
            // was already RPOP+GETDEL'd (destructively consumed), so the advert
            // signals nothing claimable either way; a crash loses exactly the
            // claimed-but-unfinished strategies, same as before. A removal
            // failure propagates and aborts the loop, we never re-peek the same
            // run and reload its ticks in a tight loop.
            co_await run_queue::removeRun(conn, peeked->queueKey,
                                          peeked->descriptorB64);

            std::println("RedisRunner: drained RUN_ID={} ({} strateg{} queued, backtests pipelined)",
                         runCfg.RUN_ID, strategiesRun,
                         strategiesRun == 1 ? "y" : "ies");
        }

        // The loop only breaks for a stop request (exceptions unwind past this
        // point into the catch blocks below). Join every pipelined backtest
        // before parking, and surface the first infrastructure failure they
        // raised — matching the abort semantics the per-run barrier used to
        // provide.
        pool.wait();
        if (std::exception_ptr err = pool.takeError()) {
            std::rethrow_exception(err);
        }

        // A stop signal drains the pool (above) and then parks the engine here:
        // alive but idle, never accepting more work. This is a one-way pause, not
        // a shutdown — the process stays up and the control segment stays mapped
        // (active_jobs = 0, stop_signal = 1) so the monitor can confirm the drain.
        if (stopping) {
            std::println("RedisRunner: stop signal received — drained and paused.");
            for (;;) {
                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
            }
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
