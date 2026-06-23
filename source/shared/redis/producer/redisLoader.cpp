// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/producer/redisLoader.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/system/system_error.hpp>

#include "shared/utilities/base64.hpp"
#include "shared/redis/connection/redisConnection.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace {

// A whitespace-only (or empty) payload is rejected before it reaches Redis.
bool isBlank(const std::string& rawJson) {
    return std::ranges::all_of(rawJson, [](unsigned char c) {
        return std::isspace(c);
    });
}

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
    if (isBlank(rawJson)) {
        std::cerr << "RedisLoader: empty payload rejected" << std::endl;
        return 1;
    }

    const std::string encoded = Base64::b64encode(rawJson);

    asio::io_context ioc;
    auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

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
                                  const int redisPort,
                                  const std::string& queueKey,
                                  const std::vector<std::string>& rawJsonPayloads) {
    if (rawJsonPayloads.empty()) {
        return 0;  // nothing to push
    }

    std::vector<std::string> encoded;
    encoded.reserve(rawJsonPayloads.size());
    for (const std::string& rawJson : rawJsonPayloads) {
        if (isBlank(rawJson)) {
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
        [&pushError](const std::exception_ptr& e) {
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
