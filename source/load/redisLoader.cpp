// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "load/redisLoader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>
#include <boost/system/system_error.hpp>

#include "shared/utilities/base64.hpp"
#include "shared/redis/connection/redisConnection.hpp"
#include "shared/redis/operations/redisOperations.hpp"

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
    RedisOperations ops(conn);
    co_await ops.listPushFront(std::move(queueKey), std::move(encoded));

    // Ephemeral connection: cancel so io_context::run() returns after the push.
    conn->cancel();
    co_return;
}

// Drains `source` chunk by chunk over the one borrowed connection. Per chunk:
// pipelined SET (PX ttl) of every payload under its own key, THEN one
// pipelined LPUSH of the key names — awaited in that order so a consumer can
// never pop a name whose payload is not yet stored. Chunk production happens
// between awaits, so memory stays bounded at one chunk. Returns the total
// number of payloads stored.
asio::awaitable<std::size_t> pushKeyedStream(
    std::shared_ptr<redis::connection> conn,
    std::string listKey,
    RedisLoader::ChunkSource& source,
    std::chrono::milliseconds ttl) {
    RedisOperations ops(conn);
    std::size_t total = 0;
    try {
        for (;;) {
            std::vector<RedisLoader::KeyedPayload> chunk = source.next();
            if (chunk.empty()) {
                break;
            }

            std::vector<std::pair<std::string, std::string>> keyedValues;
            std::vector<std::string> keyNames;
            keyedValues.reserve(chunk.size());
            keyNames.reserve(chunk.size());
            for (RedisLoader::KeyedPayload& payload : chunk) {
                if (payload.key.empty() || isBlank(payload.rawJson)) {
                    throw std::invalid_argument(
                        "RedisLoader: empty payload or key rejected");
                }
                keyNames.push_back(payload.key);
                keyedValues.emplace_back(std::move(payload.key),
                                         Base64::b64encode(payload.rawJson));
            }

            co_await ops.setMultipleWithTTL(std::move(keyedValues), ttl);
            co_await ops.listPushFront(listKey, std::move(keyNames));
            total += chunk.size();
        }
    } catch (...) {
        // Cancel on the failure path too: the connection's detached async_run
        // otherwise keeps io_context::run() from ever returning.
        conn->cancel();
        throw;
    }

    // Ephemeral connection: cancel so io_context::run() returns after the push.
    conn->cancel();
    co_return total;
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

int RedisLoader::loadKeyedPayloadStream(const std::string& redisHost,
                                        const int redisPort,
                                        const std::string& listKey,
                                        ChunkSource& source,
                                        const long ttlSeconds) {
    asio::io_context ioc;
    auto conn = redis_util::makeRedisConnection(ioc, redisHost, redisPort);

    std::exception_ptr pushError;
    std::size_t stored = 0;

    asio::co_spawn(
        ioc,
        pushKeyedStream(conn, listKey, source, std::chrono::seconds(ttlSeconds)),
        [&pushError, &stored](const std::exception_ptr& e, std::size_t total) {
            if (e) {
                pushError = e;
            } else {
                stored = total;
            }
        });

    ioc.run();

    if (pushError) {
        try {
            std::rethrow_exception(pushError);
        } catch (const boost::system::system_error& ex) {
            std::cerr << "Redis payload stream failed: " << ex.what() << std::endl;
            return 3;
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << std::endl;
            return 1;
        }
    }

    std::cout << "RedisLoader: stored " << stored
              << " keyed payload(s), key names on " << listKey << std::endl;

    return 0;
}
