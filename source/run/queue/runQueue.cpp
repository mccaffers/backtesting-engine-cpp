// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/queue/runQueue.hpp"

#include <optional>
#include <string>
#include <utility>

#include <boost/redis/connection.hpp>

#include "shared/redis/operations/redisOperations.hpp"
#include "shared/utilities/queueKeys.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace run_queue {

// All three helpers wrap a borrowed connection in a RedisOperations and delegate
// the Redis mechanics, keeping only the queue semantics (which key, FIFO peek,
// idempotent remove) here. The connection is NOT cancelled, it stays open for the
// rest of the worker loop. RedisOperations is a named local so it outlives the
// inner async awaits.

asio::awaitable<std::optional<PeekedRun>> peekQueueTail(
    std::shared_ptr<redis::connection> conn,
    std::string queueKey) {
    RedisOperations ops(std::move(conn));
    // LINDEX -1 reads the tail (oldest) run without removing it.
    if (auto descriptor = co_await ops.listIndex(queueKey, -1)) {
        co_return PeekedRun{std::move(queueKey), std::move(*descriptor)};
    }
    co_return std::nullopt;
}

asio::awaitable<std::optional<PeekedRun>> peekRunTail(
    std::shared_ptr<redis::connection> conn) {
    // First non-empty queue in priority order wins, so the chained backlog is
    // untouched while any grid (or earlier-rung) work remains.
    for (const char* queueKey : queue_keys::RUN_QUEUES) {
        if (auto peeked = co_await peekQueueTail(conn, queueKey)) {
            co_return peeked;
        }
    }
    co_return std::nullopt;
}

asio::awaitable<std::optional<std::string>> popStrategyKey(
    std::shared_ptr<redis::connection> conn,
    std::string strategyKey) {
    RedisOperations ops(std::move(conn));
    co_return co_await ops.listPopBack(std::move(strategyKey));
}

asio::awaitable<std::optional<std::string>> takeStrategyPayload(
    std::shared_ptr<redis::connection> conn,
    std::string payloadKey) {
    RedisOperations ops(std::move(conn));
    co_return co_await ops.getDelete(std::move(payloadKey));
}

asio::awaitable<void> removeRun(std::shared_ptr<redis::connection> conn,
                                std::string queueKey,
                                std::string descriptorB64) {
    RedisOperations ops(std::move(conn));
    // count 0 removes every occurrence, so retiring the run is idempotent.
    co_await ops.listRemove(std::move(queueKey), 0, std::move(descriptorB64));
}

}  // namespace run_queue
