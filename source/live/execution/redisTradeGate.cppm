// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// redisTradeGate — adapts the Redis trade lock (shared/redis/tradeLocks) into
// the strategy runner's TradeGate seam: one lock per (strategy UUID,
// direction), acquired atomically with a TTL, fail-closed on any Redis
// failure. This is the live entry LOCK gate; the runner checks the risk caps
// first (MAX_TRADES_PER_MINUTE in-memory window, MAX_OPEN_TRADES against the
// broker position count — see redisPositionCounter) so a capped signal never
// acquires a lock it cannot use.

module;

#include "shared/redis/tradeLocks.hpp"

export module redisTradeGate;

import std;
import liveStrategyRunner;  // live::TradeGate

export namespace live {

class RedisTradeGate {
public:
    // Returns a TradeGate whose callable creates one TradeLocks per calling
    // worker thread (lazily, on the thread's first signal), each destroyed at
    // that thread's exit — i.e. when the runner joins its workers.
    static TradeGate make(const std::string& redisHost, int redisPort,
                          std::chrono::seconds lockTtl);
};

}  // namespace live

namespace live {

TradeGate RedisTradeGate::make(const std::string& redisHost,
                               const int redisPort,
                               const std::chrono::seconds lockTtl) {
    return [redisHost, redisPort, lockTtl](const std::string& strategyUuid,
                                           const std::string& direction) {
        // Per-thread instance: a single shared TradeLocks would serialise
        // EVERY worker's gate check behind one mutex and one synchronous
        // pump — with Redis down that is the full op deadline paid one
        // worker at a time. SET NX is atomic server-side, so per-thread
        // connections don't weaken the lock. thread_local also means all
        // gates made by this function share one instance per thread; the
        // process only ever makes one gate, and only worker threads call it.
        thread_local std::unique_ptr<redis_locks::TradeLocks> locks;
        if (!locks) {
            locks = std::make_unique<redis_locks::TradeLocks>(redisHost,
                                                              redisPort);
        }
        return !locks->isThereATradeLock(strategyUuid, direction, lockTtl);
    };
}

}  // namespace live
