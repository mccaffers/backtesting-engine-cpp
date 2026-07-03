// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/operations/redisOperations.hpp"

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/resp3/node.hpp>
#include <boost/redis/resp3/type.hpp>

namespace asio = boost::asio;
namespace redis = boost::redis;

RedisOperations::RedisOperations(std::shared_ptr<redis::connection> conn)
    : conn_(std::move(conn)) {}

// ---- Private exec primitives ----

asio::awaitable<std::optional<std::string>> RedisOperations::execOptionalString(
    redis::request req) {
    redis::response<std::optional<std::string>> resp;
    co_await conn_->async_exec(req, resp, asio::use_awaitable);
    co_return std::move(std::get<0>(resp).value());
}

asio::awaitable<void> RedisOperations::execIgnore(redis::request req) {
    redis::generic_response resp;
    co_await conn_->async_exec(req, resp, asio::use_awaitable);
    co_return;
}

template <class T>
asio::awaitable<T> RedisOperations::execValue(redis::request req) {
    redis::response<T> resp;
    co_await conn_->async_exec(req, resp, asio::use_awaitable);
    co_return std::move(std::get<0>(resp).value());
}

// ---- Strings ----

asio::awaitable<std::optional<std::string>> RedisOperations::getString(
    std::string key) {
    redis::request req;
    req.push("GET", key);
    co_return co_await execOptionalString(std::move(req));
}

asio::awaitable<void> RedisOperations::setString(std::string key,
                                                 std::string value) {
    redis::request req;
    req.push("SET", key, value);
    co_await execIgnore(std::move(req));
}

asio::awaitable<bool> RedisOperations::setString(
    std::string key,
    std::string value,
    SetWhen when,
    std::optional<std::chrono::milliseconds> expiry) {
    // SET key value [PX ms] [NX|XX]. Built as a dynamic argument range because
    // the optional flags vary at runtime.
    std::vector<std::string> args{std::move(key), std::move(value)};
    if (expiry) {
        args.emplace_back("PX");
        args.emplace_back(std::to_string(expiry->count()));
    }
    if (when == SetWhen::NotExists) {
        args.emplace_back("NX");
    } else if (when == SetWhen::Exists) {
        args.emplace_back("XX");
    }

    redis::request req;
    req.push_range("SET", args);

    // A conditional SET replies with the stored value ("OK") on success or nil
    // when the NX/XX condition blocked the write.
    const std::optional<std::string> reply =
        co_await execOptionalString(std::move(req));
    co_return reply.has_value();
}

asio::awaitable<bool> RedisOperations::setIfNotExists(
    std::string key,
    std::string value,
    std::optional<std::chrono::milliseconds> expiry) {
    co_return co_await setString(std::move(key), std::move(value),
                                 SetWhen::NotExists, expiry);
}

asio::awaitable<void> RedisOperations::setMultiple(
    std::vector<std::pair<std::string, std::string>> keyValuePairs) {
    if (keyValuePairs.empty()) {
        co_return;
    }
    redis::request req;
    // Each pair serializes to two bulk strings (key, value): MSET k1 v1 k2 v2 ...
    req.push_range("MSET", keyValuePairs);
    co_await execIgnore(std::move(req));
}

asio::awaitable<void> RedisOperations::setMultipleWithTTL(
    std::vector<std::pair<std::string, std::string>> keyValuePairs,
    std::chrono::milliseconds ttl) {
    if (keyValuePairs.empty()) {
        co_return;
    }
    const std::string ttlMs = std::to_string(ttl.count());
    redis::request req;
    for (const auto& [key, value] : keyValuePairs) {
        req.push("SET", key, value, "PX", ttlMs);
    }
    co_await execIgnore(std::move(req));
}

asio::awaitable<std::optional<std::string>> RedisOperations::getDelete(
    std::string key) {
    redis::request req;
    req.push("GETDEL", key);
    co_return co_await execOptionalString(std::move(req));
}

asio::awaitable<std::map<std::string, std::string>>
RedisOperations::getMultiple(std::vector<std::string> keys) {
    std::map<std::string, std::string> result;
    if (keys.empty()) {
        co_return result;
    }

    redis::request req;
    req.push_range("MGET", keys);

    auto values =
        co_await execValue<std::vector<std::optional<std::string>>>(
            std::move(req));

    // MGET preserves request order; zip values back onto their keys, skipping
    // nils so missing keys are simply absent (mirrors the C# behaviour).
    for (std::size_t i = 0; i < keys.size() && i < values.size(); ++i) {
        if (values[i].has_value()) {
            result.emplace(std::move(keys[i]), std::move(*values[i]));
        }
    }
    co_return result;
}

// ---- Keys / TTL ----

asio::awaitable<bool> RedisOperations::deleteKey(std::string key) {
    redis::request req;
    req.push("DEL", key);
    co_return (co_await execValue<long long>(std::move(req))) > 0;
}

asio::awaitable<long> RedisOperations::deleteKeys(std::vector<std::string> keys) {
    if (keys.empty()) {
        co_return 0;
    }
    redis::request req;
    req.push_range("DEL", keys);
    co_return static_cast<long>(co_await execValue<long long>(std::move(req)));
}

asio::awaitable<bool> RedisOperations::setTTL(std::string key,
                                              std::chrono::milliseconds ttl) {
    redis::request req;
    req.push("PEXPIRE", key, ttl.count());
    co_return (co_await execValue<long long>(std::move(req))) != 0;
}

asio::awaitable<void> RedisOperations::keyExpire(
    std::string key,
    std::optional<std::chrono::milliseconds> expiry) {
    redis::request req;
    if (expiry) {
        req.push("PEXPIRE", key, expiry->count());
    } else {
        req.push("PERSIST", key);
    }
    co_await execIgnore(std::move(req));
}

asio::awaitable<bool> RedisOperations::keyRename(std::string sourceKey,
                                                 std::string destinationKey) {
    redis::request req;
    req.push("RENAME", sourceKey, destinationKey);
    co_await execIgnore(std::move(req));
    co_return true;
}

asio::awaitable<std::vector<std::string>> RedisOperations::getKeysByPattern(
    std::string pattern) {
    std::vector<std::string> keys;
    std::string cursor = "0";

    // SCAN replies with a nested array [next-cursor, [keys...]] which the typed
    // adapters can't model in one reply, so decode the flat RESP3 node tree:
    //   depth 0 -> the outer 2-element array
    //   depth 1 -> [0] the cursor, [1] the keys array
    //   depth 2 -> each key
    // Loop until the cursor wraps back to "0".
    do {
        redis::request req;
        req.push("SCAN", cursor, "MATCH", pattern, "COUNT", "250");

        redis::generic_response resp;
        co_await conn_->async_exec(req, resp, asio::use_awaitable);

        const auto& nodes = resp.value();
        if (nodes.size() < 2) {
            break;  // malformed reply; nothing more to read
        }
        cursor = nodes[1].value;  // first depth-1 element is the cursor

        for (std::size_t i = 2; i < nodes.size(); ++i) {
            if (nodes[i].depth == 2 &&
                nodes[i].data_type == redis::resp3::type::blob_string) {
                keys.push_back(nodes[i].value);
            }
        }
    } while (cursor != "0");

    co_return keys;
}

// ---- Sets ----

asio::awaitable<bool> RedisOperations::setAdd(std::string key,
                                              std::string value) {
    redis::request req;
    req.push("SADD", key, value);
    co_return (co_await execValue<long long>(std::move(req))) != 0;
}

asio::awaitable<bool> RedisOperations::setRemove(std::string key,
                                                 std::string value) {
    redis::request req;
    req.push("SREM", key, value);
    co_return (co_await execValue<long long>(std::move(req))) != 0;
}

asio::awaitable<std::vector<std::string>> RedisOperations::setMembers(
    std::string key) {
    redis::request req;
    req.push("SMEMBERS", key);
    co_return co_await execValue<std::vector<std::string>>(std::move(req));
}

asio::awaitable<bool> RedisOperations::setContains(std::string key,
                                                   std::string value) {
    redis::request req;
    req.push("SISMEMBER", key, value);
    co_return (co_await execValue<long long>(std::move(req))) != 0;
}

asio::awaitable<long> RedisOperations::setLength(std::string key) {
    redis::request req;
    req.push("SCARD", key);
    co_return static_cast<long>(co_await execValue<long long>(std::move(req)));
}

// ---- Lists ----

asio::awaitable<void> RedisOperations::listPushFront(std::string key,
                                                     std::string value) {
    redis::request req;
    req.push("LPUSH", key, value);
    co_await execIgnore(std::move(req));
}

asio::awaitable<void> RedisOperations::listPushFront(
    std::string key,
    std::vector<std::string> values) {
    redis::request req;
    for (const std::string& value : values) {
        req.push("LPUSH", key, value);
    }
    co_await execIgnore(std::move(req));
}

asio::awaitable<std::optional<std::string>> RedisOperations::listPopBack(
    std::string key) {
    redis::request req;
    req.push("RPOP", key);
    co_return co_await execOptionalString(std::move(req));
}

asio::awaitable<std::optional<std::string>> RedisOperations::listIndex(
    std::string key,
    long index) {
    redis::request req;
    req.push("LINDEX", key, index);
    co_return co_await execOptionalString(std::move(req));
}

asio::awaitable<void> RedisOperations::listRemove(std::string key,
                                                  long count,
                                                  std::string value) {
    redis::request req;
    req.push("LREM", key, count, value);
    co_await execIgnore(std::move(req));
}

// ---- Generic ----

asio::awaitable<redis::generic_response> RedisOperations::execute(
    std::vector<std::string> commandAndArgs) {
    redis::generic_response resp;
    if (commandAndArgs.empty()) {
        co_return resp;
    }

    redis::request req;
    if (commandAndArgs.size() == 1) {
        req.push(commandAndArgs[0]);
    } else {
        req.push_range(commandAndArgs[0], commandAndArgs.begin() + 1,
                       commandAndArgs.end());
    }

    co_await conn_->async_exec(req, resp, asio::use_awaitable);
    co_return resp;
}
