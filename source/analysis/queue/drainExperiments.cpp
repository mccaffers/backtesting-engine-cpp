// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "analysis/queue/drainExperiments.hpp"

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
#include "run/reporting/elasticPublisher.hpp"
#include "run/reporting/tradingResults.hpp"  // TradeFinal::localHostname
#include "run/queue/runQueue.hpp"
#include "analysis/queue/analysisBridge.hpp"
#include "analysis/reporting/experimentResults.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace analysis_runner {

asio::awaitable<int> drainExperiments(std::shared_ptr<redis::connection> conn,
                                      std::string questdbHost) {
    int exitCode = 0;

    try {
        // Same pool sizing as drainRuns: 80% of the available CPU threads,
        // at least one worker. Deliberately no shm control channel in v1 —
        // Ctrl+C is the only stop.
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned threads = hw != 0 ? std::max(1u, hw * 4 / 5) : 1u;
        ThreadPool pool(threads);

        const std::string hostname = TradeFinal::localHostname();

        // Loop forever, claiming one experiment run per iteration. An empty
        // queue makes us wait and re-peek; only an exception blows the loop.
        bool waitingLogged = false;
        for (;;) {
            // Surface the first infrastructure-shaped task failure from any
            // still-pipelining evaluation (experiment-scoped errors are
            // contained inside the tasks themselves).
            if (std::exception_ptr err = pool.takeError()) {
                std::rethrow_exception(err);
            }
            const std::optional<run_queue::PeekedRun> peeked =
                co_await run_queue::peekQueueTail(conn,
                                                  queue_keys::EXPERIMENT_RUN);
            if (!peeked.has_value()) {
                // Queue empty: stay alive and poll until work reappears — a
                // co_awaited timer, so this suspends rather than busy-waits.
                if (!waitingLogged) {
                    std::println(
                        "AnalysisRunner: experiment queue empty, waiting for work...");
                    waitingLogged = true;
                }

                asio::steady_timer timer(co_await asio::this_coro::executor);
                timer.expires_after(std::chrono::seconds(1));
                co_await timer.async_wait(asio::use_awaitable);
                continue;  // re-peek; never exit just because the queue is empty
            }
            waitingLogged = false;

            // An unparseable run descriptor is a poison pill: retire it
            // (removeRun LREMs by the raw base64 value, so no parse needed)
            // instead of letting it kill this worker and the next.
            experiments::ExperimentRunConfiguration runCfg;
            bool descriptorOk = true;
            try {
                runCfg = JsonParser::parseExperimentRunFromBase64(
                    peeked->descriptorB64);
            } catch (const std::exception& ex) {
                // co_await is illegal inside a catch handler, so the removal
                // happens just below, outside the try/catch.
                std::println(stderr,
                             "AnalysisRunner: unparseable experiment run descriptor, retiring: {}",
                             ex.what());
                elastic::putEngineException(
                    {elastic::nowIsoUtc(), "drainExperiments",
                     std::string("unparseable experiment run descriptor retired: ")
                         + ex.what(),
                     ""});
                descriptorOk = false;
            }
            if (!descriptorOk) {
                co_await run_queue::removeRun(conn, peeked->queueKey,
                                              peeked->descriptorB64);
                continue;
            }
            const std::string experimentKey =
                queue_keys::experimentKey(runCfg.RUN_ID);

            std::println("drainExperiments, RUN_ID={} SYMBOLS={} LAST_MONTHS={} OFFSET_MONTHS={} EXPERIMENT_KEY={}",
                         runCfg.RUN_ID, runCfg.SYMBOLS, runCfg.LAST_MONTHS,
                         runCfg.OFFSET_MONTHS, experimentKey);

            // Claim the first experiment BEFORE the expensive tick load: when
            // several workers converge on a nearly-drained run, the losers
            // would otherwise each pay a full QuestDB fetch only to find the
            // list already empty.
            std::optional<std::string> payloadKey =
                co_await run_queue::popStrategyKey(conn, experimentKey);
            if (!payloadKey.has_value()) {
                co_await run_queue::removeRun(conn, peeked->queueKey,
                                              peeked->descriptorB64);
                std::println("AnalysisRunner: RUN_ID={} already drained, retiring",
                             runCfg.RUN_ID);
                continue;
            }

            // One QuestDB load for the whole run, shared BY VALUE with every
            // pooled evaluation below — the shared handle keeps the buffer
            // alive until the last task holding it finishes.
            const ExperimentTicks ticks = bridgeLoadExperimentTicks(
                questdbHost, runCfg.SYMBOLS, runCfg.LAST_MONTHS,
                runCfg.OFFSET_MONTHS);

            // Drain the run's experiment list, competing with any other
            // workers: RPOP a payload key name, GETDEL the payload, hand the
            // CPU-bound evaluation to the pool. submit() applies
            // backpressure, so we pop at the rate the workers absorb.
            int experimentsRun = 0;
            for (;;) {
                const std::optional<std::string> experimentB64 =
                    co_await run_queue::takeStrategyPayload(conn, *payloadKey);
                if (!experimentB64.has_value()) {
                    // The safety-net TTL reaped a long-stale payload; skip
                    // it — the run still drains.
                    std::println(stderr,
                                 "AnalysisRunner: payload {} missing (expired?), skipping",
                                 *payloadKey);
                } else {
                    // A payload that fails to parse is a poison pill: report
                    // it and move on — it is already consumed, so it cannot
                    // recur.
                    try {
                        experiments::ExperimentConfig experiment =
                            JsonParser::parseExperimentFromBase64(*experimentB64);
                        // Evaluation errors (e.g. a chain this binary's
                        // matcher rejects) are likewise contained per task.
                        pool.submit([ticks, runCfg, hostname,
                                     experiment = std::move(experiment)]() {
                            try {
                                const auto started =
                                    std::chrono::steady_clock::now();
                                const experiments::ExperimentOutcome outcome =
                                    bridgeEvaluateExperiment(ticks, experiment);
                                const double durationSeconds =
                                    std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - started)
                                        .count();
                                bridgePutExperimentResults(ExperimentResults{
                                    .RUN_ID = runCfg.RUN_ID,
                                    .timestamp = elastic::nowIsoUtc(),
                                    .hostname = hostname,
                                    .durationSeconds = durationSeconds,
                                    .runConfig = runCfg,
                                    .experiment = experiment,
                                    .occurrences = outcome.occurrences,
                                    .ticksScanned = outcome.ticksScanned,
                                    .daysSpanned = outcome.daysSpanned,
                                    .occurrencesPerDay =
                                        outcome.daysSpanned > 0.0
                                            ? outcome.occurrences /
                                                  outcome.daysSpanned
                                            : 0.0,
                                    .attempts = outcome.attempts,
                                    .failuresByLeg = outcome.failuresByLeg,
                                    .occurrencesByMonth =
                                        outcome.occurrencesByMonth,
                                    .occurrencesByHourUtc =
                                        outcome.occurrencesByHourUtc,
                                    .perSymbol = outcome.perSymbol,
                                    .occurrencesBySymbol =
                                        outcome.occurrencesBySymbol,
                                    .completedAttempts =
                                        outcome.completedAttempts,
                                    .failedAttempts = outcome.failedAttempts,
                                    .completionSeconds =
                                        outcome.completionSeconds,
                                    .meanSpreadAtTriggerPoints =
                                        outcome.meanSpreadAtTriggerPoints,
                                    .excursionOrientation =
                                        outcome.excursionOrientation,
                                    .samplesTruncated =
                                        outcome.samplesTruncated,
                                });
                            } catch (const std::exception& ex) {
                                std::println(stderr,
                                             "AnalysisRunner: experiment failed (RUN_ID={} experiment={}): {}",
                                             runCfg.RUN_ID, experiment.UUID,
                                             ex.what());
                                elastic::putEngineException(
                                    {elastic::nowIsoUtc(), "experiment",
                                     std::string("experiment ") + experiment.UUID
                                         + " failed: " + ex.what(),
                                     runCfg.RUN_ID});
                            }
                        });
                        ++experimentsRun;
                    } catch (const std::exception& ex) {
                        std::println(stderr,
                                     "AnalysisRunner: unparseable experiment payload {} (RUN_ID={}), skipping: {}",
                                     *payloadKey, runCfg.RUN_ID, ex.what());
                        elastic::putEngineException(
                            {elastic::nowIsoUtc(), "drainExperiments",
                             std::string("unparseable experiment payload skipped: ")
                                 + ex.what(),
                             runCfg.RUN_ID});
                    }
                }

                payloadKey = co_await run_queue::popStrategyKey(conn, experimentKey);
                if (!payloadKey.has_value()) {
                    break;  // experiment list drained
                }
            }

            // Retire the run as soon as its list is drained — evaluations may
            // still be pipelining on the pool, but every payload was already
            // destructively consumed, so the advert signals nothing claimable
            // either way. LREM is idempotent across competing workers.
            co_await run_queue::removeRun(conn, peeked->queueKey,
                                          peeked->descriptorB64);

            std::println("AnalysisRunner: drained RUN_ID={} ({} experiment{} queued, evaluations pipelined)",
                         runCfg.RUN_ID, experimentsRun,
                         experimentsRun == 1 ? "" : "s");
        }
    } catch (const std::exception& ex) {
        std::println(stderr, "AnalysisRunner aborted: {}", ex.what());
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "drainExperiments", ex.what(), ""});
        exitCode = 3;
    } catch (...) {
        std::println(stderr, "AnalysisRunner aborted: unknown error");
        elastic::putEngineException(
            {elastic::nowIsoUtc(), "drainExperiments", "unknown error", ""});
        exitCode = 3;
    }

    // Only reached when an exception aborted the drain — the loop waits on an
    // empty queue rather than exiting. Tear the connection down so
    // io_context::run() can return.
    conn->cancel();
    co_return exitCode;
}

}  // namespace analysis_runner
