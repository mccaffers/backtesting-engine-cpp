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

asio::awaitable<std::optional<std::string>> peekRunTail(
    std::shared_ptr<redis::connection> conn) {
    RedisOperations ops(std::move(conn));
    // LINDEX -1 reads the tail (oldest) run without removing it.
    co_return co_await ops.listIndex(queue_keys::RUN, -1);
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
                                std::string descriptorB64) {
    RedisOperations ops(std::move(conn));
    // count 0 removes every occurrence, so retiring the run is idempotent.
    co_await ops.listRemove(queue_keys::RUN, 0, std::move(descriptorB64));
}

}  // namespace run_queue
