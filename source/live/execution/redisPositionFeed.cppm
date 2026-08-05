// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// redisPositionFeed — adapts the Redis broker position store (PL#/PO#, see
// shared/redis/positionManager) into the strategy runner's PositionFeed
// seam: the deals one (strategy UUID, symbol) worker should mirror into its
// TradeManager book. Same per-worker-thread pattern as RedisPositionCounter,
// but NO cache here — the runner's bookSyncInterval already bounds the call
// rate to one fetch per worker per interval.
//
// Payloads that fail to decode are logged and SKIPPED: to the sync that
// reads as closed-at-broker, which is safe — a removal sends no order, and
// re-entry stays guarded by the caps and the trade lock.

module;

#include "shared/redis/positionManager.hpp"

export module redisPositionFeed;

import std;                 // replaces <cmath>, <memory>, <optional>, <string>, <vector>
import backtestLog;         // backtest_log::logLine
import liveStrategyRunner;  // live::PositionFeed, BookedPosition
import marketDefinitions;   // live::findMarket
import trade;               // Direction

export namespace live {

// Pure record -> book-entry mapping, exported for tests. nullopt when the
// direction is neither BUY nor SELL (an unaddressable deal must not enter
// the book). `market` may be null (symbol since dropped from the table):
// the broker size then passes through unscaled, loudly.
[[nodiscard]] std::optional<BookedPosition> toBookedPosition(
    const redis_positions::PositionRecord& record,
    const MarketDefinition* market);

class RedisPositionFeed {
public:
    // Returns a PositionFeed whose callable creates one PositionManager per
    // calling worker thread (lazily, on the thread's first sync), each
    // destroyed at that thread's exit — the same per-thread pattern, for
    // the same serialisation reasons, as RedisTradeGate.
    static PositionFeed make(const std::string& redisHost, int redisPort);
};

}  // namespace live

namespace live {

std::optional<BookedPosition> toBookedPosition(
    const redis_positions::PositionRecord& record,
    const MarketDefinition* market) {
    Direction direction{};
    if (record.direction == "BUY") {
        direction = Direction::LONG;
    } else if (record.direction == "SELL") {
        direction = Direction::SHORT;
    } else {
        return std::nullopt;
    }
    // Engine lots from the broker size by REVERSING the trade-size modifier
    // — truthful to the actual position (a deal opened under an older
    // config with a different TRADING_SIZE must not be misbooked from the
    // current spec). The broker size itself stays verbatim: it is what a
    // close must send.
    const double modifier = market != nullptr ? market->sizeModifier() : 1.0;
    if (market == nullptr) {
        backtest_log::logLine(
            "RedisPositionFeed: {} missing from marketDefinitions — booking "
            "{} with its broker size unscaled",
            record.symbol, record.dealReference);
    }
    return BookedPosition{
        .dealId = record.dealId,
        .dealReference = record.dealReference,
        .direction = direction,
        .brokerSize = record.size,
        .engineSize = static_cast<std::int32_t>(
            std::llround(record.size / modifier)),
        .level = record.level,
        .openedAt = std::chrono::system_clock::time_point{
            std::chrono::microseconds{record.openedAtMicros}},
    };
}

PositionFeed RedisPositionFeed::make(const std::string& redisHost,
                                     const int redisPort) {
    return [redisHost, redisPort](const std::string& strategyUuid,
                                  const std::string& symbol)
               -> std::optional<std::vector<BookedPosition>> {
        thread_local std::unique_ptr<redis_positions::PositionManager> manager;
        if (!manager) {
            manager = std::make_unique<redis_positions::PositionManager>(
                redisHost, redisPort);
        }
        const auto payloads = manager->getPositionPayloads(strategyUuid);
        if (!payloads) {
            return std::nullopt;  // UNKNOWN — the runner keeps its book
        }
        std::vector<BookedPosition> positions;
        positions.reserve(payloads->size());
        for (const auto& [reference, payload] : *payloads) {
            const auto record =
                redis_positions::decodePositionRecord(payload);
            if (!record) {
                backtest_log::logLine(
                    "RedisPositionFeed: undecodable PO# payload for {} "
                    "({} bytes) — skipping (reads as closed-at-broker)",
                    reference, payload.size());
                continue;
            }
            // A UUID may trade several symbols across workers — each
            // worker's book mirrors only its own symbol's deals.
            if (record->symbol != symbol) {
                continue;
            }
            if (auto position = toBookedPosition(*record, findMarket(symbol))) {
                positions.push_back(std::move(*position));
            } else {
                backtest_log::logLine(
                    "RedisPositionFeed: {} has unusable direction '{}' — "
                    "skipping", reference, record->direction);
            }
        }
        return positions;
    };
}

}  // namespace live
