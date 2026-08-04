// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// redisPositionCounter — adapts the Redis broker position store
// (shared/redis/positionManager, the PL#/PO# keys an external producer
// refreshes from the broker every ~2 minutes) into the strategy runner's
// PositionCounter seam, for the MAX_OPEN_TRADES entry cap. Counts are cached
// per thread for a short period: a per-tick signaller (RandomStrategy) would
// otherwise pay a Redis GET per tick, and anything fresher than the
// producer's own 2-minute cadence is precision the data doesn't have.
//
// The one event that DOES move the count between producer refreshes is this
// worker's own accepted open (the channel's addPosition lands in PL#
// immediately) — so brokerOrderSink invalidates the cache after each
// successful open/close via invalidateCache(). Without that, a LONG open
// followed by a SHORT signal inside the cache TTL read the stale pre-open
// count and breached MAX_OPEN_TRADES (the trade lock is per-direction, so it
// never serialised the two).

module;

#include "shared/redis/positionManager.hpp"

export module redisPositionCounter;

import std;
import liveStrategyRunner;  // live::PositionCounter

export namespace live {

// The per-uuid count cache, pure and clock-free (`now`/`freshUntil` are
// passed in) so tests drive expiry without waiting. Exported for unit tests;
// the runtime instance is a per-worker-thread thread_local below.
class PositionCountCache {
public:
    // The cached count, or nullopt when absent or no longer fresh at `now`.
    [[nodiscard]] std::optional<int> get(
        const std::string& strategyUuid,
        const std::chrono::steady_clock::time_point now) const {
        const auto it = cache_.find(strategyUuid);
        if (it == cache_.end() || now >= it->second.freshUntil) {
            return std::nullopt;
        }
        return it->second.count;
    }

    void put(const std::string& strategyUuid, const int count,
             const std::chrono::steady_clock::time_point freshUntil) {
        cache_[strategyUuid] = CachedCount{count, freshUntil};
    }

    void invalidate(const std::string& strategyUuid) {
        cache_.erase(strategyUuid);
    }

private:
    struct CachedCount {
        int count;
        std::chrono::steady_clock::time_point freshUntil;
    };
    std::map<std::string, CachedCount> cache_;
};

class RedisPositionCounter {
public:
    // Returns a PositionCounter whose callable creates one PositionManager
    // per calling worker thread (lazily, on the thread's first capped
    // signal), each destroyed at that thread's exit — the same per-thread
    // pattern, for the same serialisation reasons, as RedisTradeGate.
    static PositionCounter make(
        const std::string& redisHost, int redisPort,
        std::chrono::seconds cacheTtl = std::chrono::seconds{15});

    // Drops the CALLING thread's cached count for the strategy, forcing the
    // next cap check to re-read PL#. Called by brokerOrderSink right after
    // an accepted open / successful close — same worker thread as the cap
    // check, so the invalidation always hits the cache that matters. A
    // thread that never cached the uuid is a no-op.
    static void invalidateCache(const std::string& strategyUuid);
};

}  // namespace live

namespace live {

namespace {

// One cache per thread, shared by every counter make() returns and by
// invalidateCache(). The pre-extraction code kept this as a thread_local
// inside make()'s lambda, which is the same sharing (one instance per
// thread across all closures) — it just had no invalidation handle.
PositionCountCache& threadCache() {
    thread_local PositionCountCache cache;
    return cache;
}

}  // namespace

PositionCounter RedisPositionCounter::make(const std::string& redisHost,
                                           const int redisPort,
                                           const std::chrono::seconds cacheTtl) {
    return [redisHost, redisPort,
            cacheTtl](const std::string& strategyUuid) -> std::optional<int> {
        thread_local std::unique_ptr<redis_positions::PositionManager> manager;
        if (!manager) {
            manager = std::make_unique<redis_positions::PositionManager>(
                redisHost, redisPort);
        }
        // Only SUCCESSFUL reads are cached — an unknown count must stay
        // unknown (fail closed) rather than serve a stale number, and the
        // client's circuit breaker already fail-fasts repeat probes while
        // Redis is down. Staleness up to cacheTtl cannot over-open on its
        // own: this worker's own opens invalidate (see invalidateCache),
        // and any other writer republishes on the producer cadence anyway.
        const auto now = std::chrono::steady_clock::now();
        if (const std::optional<int> cached =
                threadCache().get(strategyUuid, now)) {
            return cached;
        }
        const std::optional<int> count =
            manager->getPositionCount(strategyUuid);
        if (count) {
            threadCache().put(strategyUuid, *count, now + cacheTtl);
        }
        return count;
    };
}

void RedisPositionCounter::invalidateCache(const std::string& strategyUuid) {
    threadCache().invalidate(strategyUuid);
}

}  // namespace live
