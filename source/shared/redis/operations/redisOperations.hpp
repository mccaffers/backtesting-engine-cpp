// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>

// Conditional-set semantics for SET, the analogue of StackExchange.Redis' When.
// Always issues a plain SET; NotExists/Exists add the NX/XX flag.
enum class SetWhen { Always, NotExists, Exists };

// Thin command layer over a borrowed Boost.Redis connection. Centralizes
// request building, async_exec and reply decoding so no caller hardcodes a Redis
// command string. Modelled on the C# IRedisOperations surface, adapted to the
// Boost.Redis coroutine model: there is no connection pool, CommandFlags or
// IBatch here (Boost.Redis multiplexes one connection per io_context and batches
// by pushing multiple commands into a single request). This class does NOT own
// the connection lifecycle, it never calls cancel(); creation lives in
// redis_util::makeRedisConnection and teardown stays with the caller.
class RedisOperations final {
public:
    explicit RedisOperations(std::shared_ptr<boost::redis::connection> conn);

    // ---- Strings ----

    // GET key. nullopt when the key is missing.
    boost::asio::awaitable<std::optional<std::string>> getString(std::string key);

    // SET key value (unconditional).
    boost::asio::awaitable<void> setString(std::string key, std::string value);

    // SET key value [PX ms] [NX|XX]. Returns true when the value was stored
    // (false when a NotExists/Exists condition prevented it).
    boost::asio::awaitable<bool> setString(
        std::string key,
        std::string value,
        SetWhen when,
        std::optional<std::chrono::milliseconds> expiry);

    // SET key value NX [PX ms]. Returns true when the key did not already exist.
    boost::asio::awaitable<bool> setIfNotExists(
        std::string key,
        std::string value,
        std::optional<std::chrono::milliseconds> expiry = std::nullopt);

    // MSET k1 v1 k2 v2 ... (one round trip, no expiry).
    boost::asio::awaitable<void> setMultiple(
        std::vector<std::pair<std::string, std::string>> keyValuePairs);

    // SET k v PX ms for every pair, pipelined into one request (one round
    // trip). MSET cannot attach a TTL, hence per-key SETs.
    boost::asio::awaitable<void> setMultipleWithTTL(
        std::vector<std::pair<std::string, std::string>> keyValuePairs,
        std::chrono::milliseconds ttl);

    // GETDEL key: atomically read and remove, so a value handed to one caller
    // can never be observed by another. nullopt when the key is missing.
    // Requires Redis >= 6.2.
    boost::asio::awaitable<std::optional<std::string>> getDelete(
        std::string key);

    // MGET k1 k2 ... Missing keys are omitted from the result map.
    boost::asio::awaitable<std::map<std::string, std::string>> getMultiple(
        std::vector<std::string> keys);

    // ---- Keys / TTL ----

    // DEL key. true when the key existed and was removed.
    boost::asio::awaitable<bool> deleteKey(std::string key);

    // DEL k1 k2 ... Returns the number of keys actually removed.
    boost::asio::awaitable<long> deleteKeys(std::vector<std::string> keys);

    // PEXPIRE key ms. true when the expiry was applied.
    boost::asio::awaitable<bool> setTTL(std::string key,
                                        std::chrono::milliseconds ttl);

    // PEXPIRE key ms when expiry is set, otherwise PERSIST key.
    boost::asio::awaitable<void> keyExpire(
        std::string key,
        std::optional<std::chrono::milliseconds> expiry);

    // RENAME source destination. Returns true on success (throws if source is
    // missing, matching the underlying RENAME error).
    boost::asio::awaitable<bool> keyRename(std::string sourceKey,
                                           std::string destinationKey);

    // SCAN-based key enumeration (never KEYS). Returns all keys matching pattern.
    boost::asio::awaitable<std::vector<std::string>> getKeysByPattern(
        std::string pattern);

    // ---- Sets ----

    // SADD key value. true when the member was newly added.
    boost::asio::awaitable<bool> setAdd(std::string key, std::string value);

    // SREM key value. true when the member was present and removed.
    boost::asio::awaitable<bool> setRemove(std::string key, std::string value);

    // SMEMBERS key.
    boost::asio::awaitable<std::vector<std::string>> setMembers(std::string key);

    // SISMEMBER key value.
    boost::asio::awaitable<bool> setContains(std::string key, std::string value);

    // SCARD key.
    boost::asio::awaitable<long> setLength(std::string key);

    // ---- Lists (engine work-queue ops; no C# equivalent) ----

    // LPUSH key value (prepend).
    boost::asio::awaitable<void> listPushFront(std::string key,
                                               std::string value);

    // LPUSH key v1 v2 ... as N separate entries pipelined in one request, so a
    // later RPOP yields them in insertion order.
    boost::asio::awaitable<void> listPushFront(std::string key,
                                               std::vector<std::string> values);

    // RPOP key. nullopt when the list is empty.
    boost::asio::awaitable<std::optional<std::string>> listPopBack(
        std::string key);

    // LINDEX key index. nullopt when the index is out of range.
    boost::asio::awaitable<std::optional<std::string>> listIndex(std::string key,
                                                                 long index);

    // LREM key count value.
    boost::asio::awaitable<void> listRemove(std::string key,
                                            long count,
                                            std::string value);

    // ---- Generic escape hatch (C# ExecuteAsync) ----

    // Runs an arbitrary command (first element) with its arguments and returns
    // the raw reply tree for the caller to interpret.
    boost::asio::awaitable<boost::redis::generic_response> execute(
        std::vector<std::string> commandAndArgs);

private:
    // One command returning a bulk string (or nil -> nullopt).
    boost::asio::awaitable<std::optional<std::string>> execOptionalString(
        boost::redis::request req);

    // One command whose reply is ignored.
    boost::asio::awaitable<void> execIgnore(boost::redis::request req);

    // One command decoded into a single typed reply (e.g. long long, vectors).
    template <class T>
    boost::asio::awaitable<T> execValue(boost::redis::request req);

    std::shared_ptr<boost::redis::connection> conn_;
};
