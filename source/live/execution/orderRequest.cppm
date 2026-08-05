// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// orderRequest — the C# engine's RequestObject, translated to this engine's
// integer price model: everything a placement needs, snapshotted at decision
// time, with the entry/stop/limit levels precomputed from the tick.
//
// The C# constructor divides pip distances by a scalingFactor to get price
// units; here prices are scaled INT32 points (see priceData) so the same
// conversion is `pips * pointsPerPip` (symbolScale). The levels are
// bookkeeping — the IG order itself carries pip DISTANCES (TradeOpenObj),
// but the levels go into the PO# position payload so the book can be
// inspected and marked without re-deriving them.
//
// Built from the runner's OrderIntent by makeOrderRequest, which also mints
// the deal reference — the client-generated idempotency token IG echoes back
// in the confirm stream. One reference per placement attempt; the trade lock
// already guarantees at most one attempt per (strategy, direction) per TTL
// window, so uuid-prefix + direction + tick milliseconds is collision-free.

export module orderRequest;

import std;                 // replaces <chrono>, <cstdint>, <optional>, <string>
import liveStrategyRunner;  // live::OrderIntent
import symbolScale;         // symbol_scale::get
import trade;               // Direction

export namespace live {

struct OrderRequest {
    // Identity / audit.
    std::string symbol;
    std::string strategyUuid;
    std::string strategyName;
    std::string dealReference;
    Direction direction{Direction::LONG};

    // Tick snapshot (scaled INT32 points, as decoded — see priceData).
    std::int32_t bid{};
    std::int32_t ask{};
    std::chrono::system_clock::time_point timestamp;

    // Order parameters from the winning run's config.
    std::int32_t size{};
    std::int32_t stopDistancePips{};   // 0 = leg disarmed
    std::int32_t limitDistancePips{};  // 0 = leg disarmed

    // Derived at construction (C# RequestObject constructor semantics).
    int pointsPerPip{};          // symbolScale points-per-pip
    std::int32_t level{};        // entry side: ask for LONG, bid for SHORT
    std::int32_t stopLevel{};    // meaningful only when stopDistancePips > 0
    std::int32_t limitLevel{};   // meaningful only when limitDistancePips > 0
    std::int32_t spreadPoints{};  // ask - bid, in points
};

// IG constrains deal references to [A-Za-z0-9_-] and 30 chars, so the UUID
// is reduced to its first 8 alphanumerics (no '#'/'-' vocabulary leaks in):
// "<uuid8>-<L|S><epoch-millis of the decision tick>" = 8 + 1 + 1 + 13 = 23
// chars. Deterministic from its inputs, so tests can pin the format.
[[nodiscard]] std::string makeDealReference(
    std::string_view strategyUuid, Direction direction,
    std::chrono::system_clock::time_point timestamp);

// OrderIntent -> OrderRequest. nullopt when the symbol has no entry in
// symbolScale (pointsPerPip unknown means the stop/limit levels — and the
// pip distances themselves — are meaningless): the caller logs and drops,
// same fail-loud doctrine as the ingest path scaling by kUnknown.
[[nodiscard]] std::optional<OrderRequest> makeOrderRequest(
    const OrderIntent& intent);

}  // namespace live

namespace live {

std::string makeDealReference(
    const std::string_view strategyUuid, const Direction direction,
    const std::chrono::system_clock::time_point timestamp) {
    std::string prefix;
    prefix.reserve(8);
    for (const char c : strategyUuid) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) {
            prefix.push_back(c);
            if (prefix.size() == 8) {
                break;
            }
        }
    }
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            timestamp.time_since_epoch())
                            .count();
    return std::format("{}-{}{}", prefix,
                       direction == Direction::LONG ? 'L' : 'S', millis);
}

std::optional<OrderRequest> makeOrderRequest(const OrderIntent& intent) {
    const int pointsPerPip = symbol_scale::get(intent.symbol);
    if (pointsPerPip == symbol_scale::kUnknown) {
        return std::nullopt;
    }

    OrderRequest request{
        .symbol = intent.symbol,
        .strategyUuid = intent.strategyUuid,
        .strategyName = intent.strategyName,
        .dealReference = makeDealReference(intent.strategyUuid,
                                           intent.direction, intent.timestamp),
        .direction = intent.direction,
        .bid = intent.bid,
        .ask = intent.ask,
        .timestamp = intent.timestamp,
        .size = intent.size,
        .stopDistancePips = intent.stopDistancePips,
        .limitDistancePips = intent.limitDistancePips,
        .pointsPerPip = pointsPerPip,
    };

    const std::int32_t stopPoints = intent.stopDistancePips * pointsPerPip;
    const std::int32_t limitPoints = intent.limitDistancePips * pointsPerPip;

    // Entry at the side the broker fills a market order on; stop is adverse,
    // limit is favourable — the C# RequestObject constructor, in points.
    if (intent.direction == Direction::LONG) {
        request.level = intent.ask;
        request.stopLevel = request.level - stopPoints;
        request.limitLevel = request.level + limitPoints;
    } else {
        request.level = intent.bid;
        request.stopLevel = request.level + stopPoints;
        request.limitLevel = request.level - limitPoints;
    }
    request.spreadPoints = intent.ask - intent.bid;
    return request;
}

}  // namespace live
