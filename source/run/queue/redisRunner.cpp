// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/queue/redisRunner.hpp"

#include <exception>
#include <memory>
#include <print>
#include <string>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>

#include "shared/utilities/backtestLog.hpp"
#include "run/reporting/elasticPublisher.hpp"
#include "shared/redis/connection/redisConnection.hpp"
#include "run/queue/drainRuns.hpp"

namespace asio = boost::asio;

int RedisRunner::run(const std::string& questdbHost,
                     const std::string& redisHost,
                     const int redisPort) {

    backtest_log::set_quiet(true); // Mute logs to prevent interleaved thread spam
    asio::io_context ioc;  // Set up the async event loop

    const auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

    // State trackers for the coroutine's outcome
    int result = 0;
    std::exception_ptr error;

    // Launch the async task to process runs
    asio::co_spawn(
        ioc,
        redis_runner::drainRuns(conn, questdbHost),
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
            std::println(stderr, "RedisRunner failed: {}", ex.what());
            // Surface the top-level failure in Elasticsearch alongside run
            // outcomes. No RUN_ID is in scope here — this is a whole-process
            // failure, not a single run.
            elastic::putEngineException(
                {elastic::nowIsoUtc(), "RedisRunner", ex.what(), ""});
        }
        return 3; // Exit with error status
    }

    // Return the final execution status code
    return result;
}
