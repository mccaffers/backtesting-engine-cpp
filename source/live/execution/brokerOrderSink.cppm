// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// brokerOrderSink — the live order handoff: adapts the strategy runner's
// OrderSink AND CloseSink seams onto one OrderChannel per worker thread
// (decision -> RequestObject -> IG, and strategy close -> close request),
// binding the real Redis-backed lock/position bookkeeping around the
// injected broker calls (ig::IGMarketCalls::makeLiveOpen / makeLiveClose in
// liveCommand).
//
// Same per-worker-thread pattern as RedisTradeGate / RedisPositionCounter:
// each worker lazily builds its own TradeLocks + PositionManager + channel
// on first use, so no worker's placement serialises behind another's Redis
// round trip. Both sinks route through threadChannel() below — one shared
// function-scope thread_local set — so the open and close paths on a worker
// share ONE channel (one recently-closed window, one set of Redis
// managers). The GMF only #includes Asio-free headers.

module;

#include "shared/redis/positionClustering.hpp"
#include "shared/redis/positionManager.hpp"
#include "shared/redis/tradeLocks.hpp"

export module brokerOrderSink;

import std;                 // replaces <chrono>, <memory>, <string>
import igMarkets;           // ig::PlaceOrder, PlaceClose
import backtestLog;         // backtest_log::logLine
import liveStrategyRunner;  // live::OrderSink, CloseSink, OrderIntent, CloseIntent
import liveTrace;           // live_trace::emit — live-traces documents
import marketDefinitions;   // live::findMarket
import orderChannel;        // live::OrderChannel, CloseRequest
import orderRequest;        // live::makeOrderRequest
import redisPositionCounter;  // live::RedisPositionCounter::invalidateCache
import trade;               // Direction

export namespace live {

// The two halves of the broker handoff, built together so they share
// per-thread state.
struct BrokerSinks {
    OrderSink order;
    CloseSink close;
};

class BrokerOrderSink {
public:
    // lockTtl should match the TTL the gate acquires with (liveSettings
    // lockTtl) so the in-flight extension restarts the same window.
    static BrokerSinks make(const std::string& redisHost, int redisPort,
                            std::chrono::seconds lockTtl,
                            ig::PlaceOrder placeOrder,
                            ig::PlaceClose placeClose);
};

}  // namespace live

namespace live {

namespace {

// The per-worker-thread channel both sinks share. Thread-local quartet,
// built on this worker's first use and destroyed at thread exit (when the
// runner joins its workers). The hooks capture raw pointers to the
// thread_local managers: all four live and die with this same thread, and
// the managers are constructed first / destroyed last. Function-scope thread_locals are
// one instance per thread across ALL callers of this function — exactly the
// sharing the open+close pair needs (the process only ever wires one broker
// configuration, same caveat as RedisTradeGate's per-thread locks).
OrderChannel& threadChannel(const std::string& redisHost, const int redisPort,
                            const std::chrono::seconds lockTtl,
                            const ig::PlaceOrder& placeOrder,
                            const ig::PlaceClose& placeClose) {
    thread_local std::unique_ptr<redis_locks::TradeLocks> locks;
    thread_local std::unique_ptr<redis_positions::PositionManager> positions;
    thread_local std::unique_ptr<redis_clusters::PositionClustering> clusters;
    thread_local std::unique_ptr<OrderChannel> channel;
    if (!channel) {
        locks = std::make_unique<redis_locks::TradeLocks>(redisHost,
                                                          redisPort);
        positions = std::make_unique<redis_positions::PositionManager>(
            redisHost, redisPort);
        clusters = std::make_unique<redis_clusters::PositionClustering>(
            redisHost, redisPort);
        channel = std::make_unique<OrderChannel>(
            MarketLookup{findMarket}, placeOrder, placeClose,
            OrderChannel::Hooks{
                .clusterBlocked =
                    [c = clusters.get()](const std::string& symbol,
                                         const std::string& strategyName) {
                        return c->isClusterBlocked(symbol, strategyName);
                    },
                .clusterOpened =
                    [c = clusters.get()](const std::string& symbol) {
                        c->markOpened(symbol);
                    },
                .extendLock =
                    [l = locks.get()](const std::string& uuid,
                                      const std::string& direction,
                                      const std::chrono::seconds ttl) {
                        return l->extendLock(uuid, direction, ttl);
                    },
                .releaseLock =
                    [l = locks.get()](const std::string& uuid,
                                      const std::string& direction) {
                        return l->releaseLock(uuid, direction);
                    },
                .savePosition =
                    [p = positions.get()](const std::string& reference,
                                          const std::string& payload) {
                        return p->savePosition(reference, payload);
                    },
                .addPosition =
                    [p = positions.get()](const std::string& uuid,
                                          const std::string& reference) {
                        return p->addPosition(uuid, reference);
                    },
                .saveDealReceipt =
                    [p = positions.get()](const std::string& reference,
                                          const std::string& symbol,
                                          const std::string& payload) {
                        return p->saveDealReceipt(reference, symbol,
                                                  payload);
                    },
                .removePosition =
                    [p = positions.get()](const std::string& uuid,
                                          const std::string& reference) {
                        return p->removePosition(uuid, reference);
                    },
            },
            lockTtl);
    }
    return *channel;
}

}  // namespace

BrokerSinks BrokerOrderSink::make(const std::string& redisHost,
                                  const int redisPort,
                                  const std::chrono::seconds lockTtl,
                                  ig::PlaceOrder placeOrder,
                                  ig::PlaceClose placeClose) {
    BrokerSinks sinks;
    sinks.order = [redisHost, redisPort, lockTtl, placeOrder,
                   placeClose](const OrderIntent& intent) {
        const std::optional<OrderRequest> request = makeOrderRequest(intent);
        if (!request) {
            // A winner trading a symbol symbolScale doesn't know shouldn't
            // exist (the backtest scaled its prices with the same table) —
            // fail loud, drop the order.
            backtest_log::logLine(
                "BrokerOrderSink: {} - no symbolScale entry; dropping {} "
                "order from {}",
                intent.symbol,
                intent.direction == Direction::LONG ? "LONG" : "SHORT",
                intent.strategyName);
            if (live_trace::enabled()) {
                live_trace::emit(
                    "orderDropped",
                    {.strategyUuid = intent.strategyUuid,
                     .strategyName = intent.strategyName,
                     .symbol = intent.symbol},
                    {{"reason", "noSymbolScale"},
                     {"direction", intent.direction == Direction::LONG
                                       ? "LONG"
                                       : "SHORT"}});
            }
            return;
        }
        if (live_trace::enabled()) {
            // The intent, traced AFTER the request build so it carries the
            // freshly minted dealReference every later event correlates on.
            live_trace::emit(
                "orderIntent",
                {.strategyUuid = request->strategyUuid,
                 .strategyName = request->strategyName,
                 .symbol = request->symbol,
                 .dealReference = request->dealReference},
                {{"direction", request->direction == Direction::LONG
                                   ? "LONG"
                                   : "SHORT"},
                 {"size", static_cast<std::int64_t>(request->size)},
                 {"stopDistancePips",
                  static_cast<std::int64_t>(request->stopDistancePips)},
                 {"limitDistancePips",
                  static_cast<std::int64_t>(request->limitDistancePips)},
                 {"bid", static_cast<std::int64_t>(request->bid)},
                 {"ask", static_cast<std::int64_t>(request->ask)},
                 {"level", static_cast<std::int64_t>(request->level)},
                 {"stopLevel", static_cast<std::int64_t>(request->stopLevel)},
                 {"limitLevel",
                  static_cast<std::int64_t>(request->limitLevel)}});
        }
        const bool accepted =
            threadChannel(redisHost, redisPort, lockTtl, placeOrder,
                          placeClose)
                .request(*request);
        if (accepted) {
            // The channel's addPosition just moved PL# on this very thread;
            // drop the cached count so the next MAX_OPEN_TRADES check reads
            // the true value instead of the pre-open one (a stale read here
            // let a second direction through the cap inside the cache TTL).
            RedisPositionCounter::invalidateCache(request->strategyUuid);
        }
    };
    sinks.close = [redisHost, redisPort, lockTtl, placeOrder,
                   placeClose](const CloseIntent& intent) {
        if (live_trace::enabled()) {
            live_trace::emit(
                "closeIntent",
                {.strategyUuid = intent.strategyUuid,
                 .strategyName = intent.strategyName,
                 .symbol = intent.symbol,
                 .dealReference = intent.dealReference,
                 .dealId = intent.dealId},
                {{"direction", intent.direction == Direction::LONG
                                   ? "LONG"
                                   : "SHORT"},
                 {"brokerSize", intent.brokerSize}});
        }
        const bool closed =
            threadChannel(redisHost, redisPort, lockTtl, placeOrder,
                          placeClose)
                .closePosition(CloseRequest{
                    .strategyUuid = intent.strategyUuid,
                    .strategyName = intent.strategyName,
                    .symbol = intent.symbol,
                    .direction = intent.direction,
                    .size = intent.brokerSize,
                    .dealId = intent.dealId,
                    .dealReference = intent.dealReference,
                });
        if (closed) {
            // Mirror of the open path: removePosition just shrank PL#, so a
            // freed cap slot is visible immediately instead of after the TTL.
            RedisPositionCounter::invalidateCache(intent.strategyUuid);
        }
    };
    return sinks;
}

}  // namespace live
