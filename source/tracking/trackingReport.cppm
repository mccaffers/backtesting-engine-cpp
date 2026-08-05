// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// trackingReport — the pure half of the tracking consumer, the C++ port of
// the C# trade-tracking service's enrichment (vortex trade_tracking/
// Program.cs). trackingCommand feeds each decoded deal through these
// helpers to produce the "live-trades" Elastic document:
//
//   lookupPosition        — the C# FetchPosition: history (PH#) first, live
//                           (PO#) as the fallback, via injected getters
//   computeClosePips      — the C# pip calculator, scales injected the same
//                           way positionSync::makeFreshRecord takes its
//                           priceScale (tests exercise the arithmetic, not
//                           the current symbolScale table)
//   serializeDeal         — the C# JsonSerializer.Serialize(deal) analogue
//                           for the document's raw-deal `json` field
//   buildLiveTradeDocument— the C# ElasticTradeLogs shape
//
// Everything here is I/O-free: Redis reads arrive as std::function getters,
// the env/date/symbol-fallback strings are passed in, and the return values
// are plain data — so the unit tests need no servers.
//
// The GMF only #includes Asio-free headers (the Redis connection machinery
// stays behind positionManager.cpp), so it is safe to `import std` here;
// nlohmann in a module GMF follows liveWinners.

module;

#include <nlohmann/json.hpp>

#include "shared/redis/positionManager.hpp"

export module trackingReport;

import std;
import dealPacket;  // deal_packet::Deal — the decoded 256-byte datagram

namespace {

// Absent wire doubles (NaN -> nullopt) serialize as JSON null, matching the
// C# nullable doubles.
nlohmann::json numberOrNull(const std::optional<double>& value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

// Wire strings are producer-truncated ASCII, but a garbled datagram must
// lose at most a character, never the whole document.
std::string dumpSafe(const nlohmann::json& doc) {
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

export namespace tracking_report {

// One raw payload read keyed by dealReference, with PositionManager's
// double-optional contract: outer nullopt = Redis failure (state UNKNOWN),
// inner nullopt = key missing.
using PayloadGetter =
    std::function<std::optional<std::optional<std::string>>(const std::string&)>;

// The C# FetchPosition lookup order: history (PH#) first — a DELETED deal's
// position has usually already been archived — then live (PO#) as the
// fallback. A getter that fails, misses, or returns an undecodable payload
// falls through to the next; nullopt when neither yields a decodable record.
[[nodiscard]] std::optional<redis_positions::PositionRecord> lookupPosition(
    const std::string& dealReference, const PayloadGetter& getHistory,
    const PayloadGetter& getLive) {
    for (const PayloadGetter* getter : {&getHistory, &getLive}) {
        const auto payload = (*getter)(dealReference);
        if (!payload || !payload->has_value()) {
            continue;  // Redis failure or missing key — try the next family
        }
        if (auto record = redis_positions::decodePositionRecord(**payload)) {
            return record;
        }
    }
    return std::nullopt;
}

// Pips for a closed deal, the C# `(level - position.level) * scaling * dir *
// size`. closeLevel is IG's raw decimal price off the wire (deal.level);
// openLevelPoints is the PO#/PH# record's scaled INT32 points (decimal x
// priceScale, llround — see positionSync). The close is quantised with the
// same llround so both sides round identically:
//
//   closePoints = llround(closeLevel * priceScale)
//   pips = (closePoints - openLevelPoints) / pointsPerPip * dirSign * size
//
// dirSign follows the POSITION's direction, BUY = +1 and anything else = -1
// (the C# ternary) — on a DELETED update the deal's own direction is the
// closing side, not the position's. nullopt when either scale is <= 0
// (symbol_scale::kUnknown): an unknown symbol must skip the calc, not scale
// the P&L by zero.
[[nodiscard]] std::optional<double> computeClosePips(
    const double closeLevel, const std::int32_t openLevelPoints,
    const std::string_view direction, const double size, const int priceScale,
    const int pointsPerPip) {
    if (priceScale <= 0 || pointsPerPip <= 0) {
        return std::nullopt;
    }
    const auto closePoints =
        static_cast<double>(std::llround(closeLevel * priceScale));
    const double dirSign = direction == "BUY" ? 1.0 : -1.0;
    return (closePoints - static_cast<double>(openLevelPoints)) / pointsPerPip
           * dirSign * size;
}

// All 16 Deal fields, absent optionals as JSON null — the raw-deal audit
// string the C# stored as ElasticTradeLogs.json.
[[nodiscard]] std::string serializeDeal(const deal_packet::Deal& deal) {
    const nlohmann::json doc{
        {"level", numberOrNull(deal.level)},
        {"size", numberOrNull(deal.size)},
        {"stopLevel", numberOrNull(deal.stopLevel)},
        {"limitLevel", numberOrNull(deal.limitLevel)},
        {"dealReference", deal.dealReference},
        {"dealId", deal.dealId},
        {"dealIdOrigin", deal.dealIdOrigin},
        {"epic", deal.epic},
        {"direction", deal.direction},
        {"status", deal.status},
        {"dealStatus", deal.dealStatus},
        {"currency", deal.currency},
        {"channel", deal.channel},
        {"expiry", deal.expiry},
        {"timestamp", deal.timestamp},
        {"guaranteedStop", deal.guaranteedStop},
    };
    return dumpSafe(doc);
}

// The C# ElasticTradeLogs "live-trades" document (same shape as the order
// path's auditTrade, plus the tracking-only json/level/pips fields).
// fallbackSymbol is the epic-mapped internal symbol (or the raw epic) —
// injected so this module never touches the marketDefinitions table. pips is
// pre-computed by the caller (computeClosePips); nullopt omits the field.
// Key order is nlohmann-alphabetical — unlike the PO# payload this is not a
// shared wire contract, Elastic doesn't care.
[[nodiscard]] std::string buildLiveTradeDocument(
    const deal_packet::Deal& deal,
    const std::optional<redis_positions::PositionRecord>& position,
    const std::string_view fallbackSymbol, const std::string_view env,
    const std::string_view dateIso, const std::optional<double> pips) {
    nlohmann::json doc{
        {"date", dateIso},
        {"env", env},
        {"symbol", position && !position->symbol.empty()
                       ? position->symbol
                       : std::string(fallbackSymbol)},
        {"action", deal.status},
        {"strategy", position && !position->strategyId.empty()
                         ? position->strategyId
                         : std::string("Unknown")},
        {"dealReference", deal.dealReference},
        {"json", serializeDeal(deal)},
    };
    if (!deal.dealId.empty()) {
        doc["dealId"] = deal.dealId;  // the auditTrade omit-when-empty rule
    }
    if (deal.level && *deal.level != 0.0) {
        doc["level"] = *deal.level;  // IG sends 0 on some UPDATED events
    }
    if (pips) {
        doc["pips"] = *pips;
    }
    return dumpSafe(doc);
}

}  // namespace tracking_report
