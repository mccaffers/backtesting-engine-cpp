// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "redisLoader.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis.hpp>
#include <boost/redis/connection.hpp>

#include <nlohmann/json.hpp>

#include "base64.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace {

asio::awaitable<void> pushOnce(
    std::shared_ptr<redis::connection> conn,
    std::string queueKey,
    std::string encoded) {
    redis::request req;
    req.push("LPUSH", queueKey, encoded);

    redis::generic_response resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);

    conn->cancel();
    co_return;
}

}  // namespace

int RedisLoader::load(const std::string& rawJson,
                      const std::string& redisHost,
                      int redisPort,
                      const std::string& queueKey) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(rawJson);
    } catch (const nlohmann::json::parse_error& ex) {
        std::cerr << "RedisLoader: invalid JSON payload: " << ex.what()
                  << std::endl;
        return 2;
    }

    const std::string encoded = Base64::b64encode(rawJson);

    asio::io_context ioc;
    auto conn = std::make_shared<redis::connection>(ioc);

    redis::config cfg;
    cfg.addr.host = redisHost;
    cfg.addr.port = std::to_string(redisPort);

    conn->async_run(cfg, {}, asio::consign(asio::detached, conn));

    std::exception_ptr pushError;

    asio::co_spawn(
        ioc,
        pushOnce(conn, queueKey, encoded),
        [&pushError](std::exception_ptr e) {
            if (e) {
                pushError = e;
            }
        });

    ioc.run();

    if (pushError) {
        try {
            std::rethrow_exception(pushError);
        } catch (const std::exception& ex) {
            std::cerr << "Redis LPUSH failed: " << ex.what() << std::endl;
        }
        return 3;
    }

    const auto runId = parsed.value("RUN_ID", std::string{});
    std::cout << "RedisLoader: LPUSH " << queueKey << " RUN_ID=" << runId
              << std::endl;

    return 0;
}
