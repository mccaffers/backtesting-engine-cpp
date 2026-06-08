// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "redisLoader.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis.hpp>
#include <boost/redis/connection.hpp>
#include <boost/system/system_error.hpp>

#include "base64.hpp"
#include "redisConnection.hpp"

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

// Pipelines one LPUSH per payload over a single connection (one round trip).
// LPUSH prepends, so RPOP later yields the payloads in insertion order.
asio::awaitable<void> pushManyOnce(
    std::shared_ptr<redis::connection> conn,
    std::string queueKey,
    std::vector<std::string> encodedPayloads) {
    redis::request req;
    for (const std::string& encoded : encodedPayloads) {
        req.push("LPUSH", queueKey, encoded);
    }

    redis::generic_response resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);

    conn->cancel();
    co_return;
}

}  // namespace

int RedisLoader::loadPayload(const std::string& redisHost,
                             int redisPort,
                             const std::string& queueKey,
                             const std::string& rawJson) {
    const bool isBlank = std::all_of(rawJson.begin(), rawJson.end(),
                                     [](unsigned char c) {
                                         return std::isspace(c);
                                     });
    if (isBlank) {
        std::cerr << "RedisLoader: empty payload rejected" << std::endl;
        return 1;
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
        } catch (const boost::system::system_error& ex) {
            std::cerr << "Redis LPUSH failed: " << ex.what() << std::endl;
        }
        return 3;
    }

    return 0;
}

int RedisLoader::loadPayloadBatch(const std::string& redisHost,
                                  int redisPort,
                                  const std::string& queueKey,
                                  const std::vector<std::string>& rawJsonPayloads) {
    if (rawJsonPayloads.empty()) {
        return 0;  // nothing to push
    }

    std::vector<std::string> encoded;
    encoded.reserve(rawJsonPayloads.size());
    for (const std::string& rawJson : rawJsonPayloads) {
        const bool isBlank = std::all_of(rawJson.begin(), rawJson.end(),
                                         [](unsigned char c) {
                                             return std::isspace(c);
                                         });
        if (isBlank) {
            std::cerr << "RedisLoader: empty payload rejected" << std::endl;
            return 1;
        }
        encoded.push_back(Base64::b64encode(rawJson));
    }

    asio::io_context ioc;
    auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

    std::exception_ptr pushError;

    asio::co_spawn(
        ioc,
        pushManyOnce(conn, queueKey, std::move(encoded)),
        [&pushError](std::exception_ptr e) {
            if (e) {
                pushError = e;
            }
        });

    ioc.run();

    if (pushError) {
        try {
            std::rethrow_exception(pushError);
        } catch (const boost::system::system_error& ex) {
            std::cerr << "Redis LPUSH failed: " << ex.what() << std::endl;
        }
        return 3;
    }

    std::cout << "RedisLoader: LPUSH " << rawJsonPayloads.size()
              << " payload(s) to " << queueKey << std::endl;

    return 0;
}
