// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "redisRunner.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>

#include "backtestRunner.hpp"
#include "jsonParser.hpp"
#include "queueKeys.hpp"
#include "redisConnection.hpp"

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
asio::awaitable<std::optional<std::string>> peekRunTail(
    std::shared_ptr<redis::connection> conn) {
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
// run. Any Redis/DB/decode error aborts the whole loop (return 3) rather than
// risking a tight re-peek/reload loop. Returns 2 if there was no work at all.
asio::awaitable<int> drainRuns(std::shared_ptr<redis::connection> conn,
                               std::string questdbHost) {
    int runsProcessed = 0;
    int exitCode = 0;

    try {
        for (;;) {
            const std::optional<std::string> descriptorB64 =
                co_await peekRunTail(conn);
            if (!descriptorB64.has_value()) {
                break;  // BACKTESTING_QUEUE_RUN is empty, no work left.
            }

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

            // Drain the run's strategy list, competing with any other workers.
            int strategiesRun = 0;
            for (;;) {
                const std::optional<std::string> strategyB64 =
                    co_await popStrategy(conn, strategyKey);
                if (!strategyB64.has_value()) {
                    break;  // strategy list drained
                }

                // Reassemble the Configuration the rest of the pipeline expects.
                const trading_definitions::Configuration config{
                    runCfg.RUN_ID,
                    runCfg.SYMBOLS,
                    runCfg.LAST_MONTHS,
                    JsonParser::parseStrategyFromBase64(*strategyB64),
                };
                runBacktestOnTicks(ticks, config);
                ++strategiesRun;
            }

            // Retire the run. A failure here propagates and aborts the loop, we
            // never re-peek the same run and reload its ticks in a tight loop.
            co_await removeRun(conn, *descriptorB64);

            std::cout << "RedisRunner: completed RUN_ID=" << runCfg.RUN_ID << " ("
                      << strategiesRun << " strateg"
                      << (strategiesRun == 1 ? "y" : "ies") << ")" << std::endl;
            ++runsProcessed;
        }
    } catch (const std::exception& ex) {
        std::cerr << "RedisRunner aborted: " << ex.what() << std::endl;
        exitCode = 3;
    } catch (...) {
        std::cerr << "RedisRunner aborted: unknown error" << std::endl;
        exitCode = 3;
    }

    // Always tear the connection down so io_context::run() can return.
    conn->cancel();

    if (exitCode != 0) {
        co_return exitCode;
    }
    if (runsProcessed == 0) {
        std::cerr << "BACKTESTING_QUEUE_RUN empty, nothing to run" << std::endl;
        co_return 2;
    }
    co_return 0;
}

}  // namespace

int RedisRunner::run(const std::string& questdbHost,
                     const std::string& redisHost,
                     int redisPort) {
    asio::io_context ioc;
    auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

    int result = 0;
    std::exception_ptr error;

    asio::co_spawn(
        ioc,
        drainRuns(conn, questdbHost),
        [&result, &error](std::exception_ptr e, int r) {
            if (e) {
                error = e;
                return;
            }
            result = r;
        });

    ioc.run();

    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& ex) {
            std::cerr << "RedisRunner failed: " << ex.what() << std::endl;
        }
        return 3;
    }

    return result;
}
