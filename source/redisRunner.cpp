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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>
// Single TU that compiles the Boost.Redis implementation.
#include <boost/redis/src.hpp>

#include "backtestRunner.hpp"
#include "jsonParser.hpp"
#include "redisConnection.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace {

asio::awaitable<std::optional<std::string>> popOnce(
    std::shared_ptr<redis::connection> conn,
    std::string queueKey) {
    redis::request req;
    req.push("RPOP", queueKey);

    redis::response<std::optional<std::string>> resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);

    // Tear down the connection loop now that the single command is done.
    conn->cancel();

    co_return std::move(std::get<0>(resp).value());
}

}  // namespace

int RedisRunner::run(const std::string& questdbHost,
                     const std::string& redisHost,
                     int redisPort,
                     const std::string& queueKey) {
    asio::io_context ioc;
    auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

    std::optional<std::string> popped;
    std::exception_ptr popError;

    asio::co_spawn(
        ioc,
        popOnce(conn, queueKey),
        [&popped, &popError](std::exception_ptr e,
                             std::optional<std::string> r) {
            if (e) {
                popError = e;
                return;
            }
            popped = std::move(r);
        });

    ioc.run();

    if (popError) {
        try {
            std::rethrow_exception(popError);
        } catch (const std::exception& ex) {
            std::cerr << "Redis pop failed: " << ex.what() << std::endl;
        }
        return 3;
    }

    if (!popped.has_value()) {
        std::cerr << "strategy_queue empty — nothing to run" << std::endl;
        return 2;
    }

    std::cout << "Popped strategy from Redis (RUN_ID will follow after decode)"
              << std::endl;

    auto config = JsonParser::parseConfigurationFromBase64(*popped);

    std::cout << "Redis pop succeeded for RUN_ID=" << config.RUN_ID
              << std::endl;

    return runBacktest(questdbHost, config);
}
