// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace redis_util {
class SyncRedisClient;
}

// Redis-backed trade locks for live trading, mirroring the C# engine's
// TradeLocks: one lock per (strategy UUID, direction) with a TTL, acquired
// atomically via SET NX PX. The lock is the live entry gate — while it is
// held, the same strategy may not fire another order in the same direction,
// which also throttles strategies that signal on every tick (RandomStrategy)
// to one order per direction per TTL window.
//
// This header is Asio-free on purpose (same isolation pattern as
// redisConnection.hpp / udpReceiver.hpp): the connection machinery lives in
// redis_util::SyncRedisClient behind tradeLocks.cpp, so module global module
// fragments can #include this safely alongside `import std`.
namespace redis_locks {

// "LOCK#<strategyUuid>#<LONG|SHORT>" — key format shared with the C# engine's
// TradeLocks; a free function so tests can pin the format without a server.
std::string lockKey(const std::string& strategyUuid,
                    const std::string& direction);

class TradeLocks {
public:
    // Connects lazily on first use via the shared Boost.Redis connection
    // (host from $REDIS_HOST in the caller; port 6379 by convention).
    TradeLocks(const std::string& host, int port);
    ~TradeLocks();
    TradeLocks(const TradeLocks&) = delete;
    TradeLocks& operator=(const TradeLocks&) = delete;

    // SET key NX PX(ttl). Returns TRUE when a lock ALREADY existed (do not
    // trade); acquires the lock and returns false otherwise. FAIL-CLOSED: any
    // Redis failure (unreachable, timeout, dead connection) is logged and
    // reported as "locked" — in live trading a missed entry is recoverable,
    // a duplicate or ungated order is not, so uncertainty must block.
    bool isThereATradeLock(const std::string& strategyUuid,
                           const std::string& direction,
                           std::chrono::seconds ttl = std::chrono::seconds{30});

    // SET key PX(ttl), UNCONDITIONAL — restarts the lock's TTL from now,
    // mirroring the C# TradeLocks extend (a plain SET with expiry overwrites
    // whatever TTL remained). For the broker order channel: keep the lock
    // alive while an order is in flight so it cannot lapse mid-placement.
    // Returns false when Redis was unreachable — the lock then simply lapses
    // at its previous TTL (logged; recoverable).
    bool extendLock(const std::string& strategyUuid,
                    const std::string& direction,
                    std::chrono::seconds ttl = std::chrono::seconds{30});

    // DEL key — drops the lock early (C# TradeLocks release semantics), so
    // the strategy may re-enter before the TTL runs out, e.g. once the broker
    // confirms the order was rejected. Returns false when Redis was
    // unreachable — the lock then expires on its own TTL (logged; safe, just
    // slower to reopen).
    bool releaseLock(const std::string& strategyUuid,
                     const std::string& direction);

private:
    std::unique_ptr<redis_util::SyncRedisClient> client_;
    // Locks THIS instance acquired, mapped to the steady-clock instant their
    // TTL is guaranteed to still cover — while a lock is known-held the check
    // answers locally instead of paying a Redis round trip per signal.
    std::map<std::string, std::chrono::steady_clock::time_point> heldUntil_;
    // Serialises concurrent callers sharing one instance (one connection, one
    // synchronous pump). The live gate avoids the contention entirely by
    // giving each worker thread its own TradeLocks (see RedisTradeGate).
    std::mutex mutex_;
};

}  // namespace redis_locks
