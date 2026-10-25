// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// tickLine — formats one decoded tick as a QuestDB ILP line for the ingest.
//
//   {symbol},source={IG|MX} ask={..}i,bid={..}i {tsNanos}\n
//
// table = symbol, `source` is an ILP tag (so QuestDB stores it as a SYMBOL
// column, added to each table on its first tagged line), integer fields
// ask/bid, and the designated timestamp in nanoseconds (QuestDB HTTP default
// precision). This yields the ask/bid/timestamp columns the read path expects,
// plus `source`.
//
// Only the known feed ids are written as tags. An empty or unrecognised source
// is written without the tag (QuestDB stores NULL), which keeps arbitrary bytes
// out of the line (tag values would otherwise need ILP escaping) and stops a
// stray value becoming a permanent entry in the SYMBOL table.

export module tickLine;

import std;         // <chrono>, <format>, <string>, <string_view>
import priceData;   // PriceData
import tickPacket;  // tick_packet::DecodedTick, kSourceIG, kSourceMassive

export namespace tick_line {

// True for the feed ids the producers send ("IG", "MX").
[[nodiscard]] inline bool isKnownSource(std::string_view source) noexcept {
    return source == tick_packet::kSourceIG || source == tick_packet::kSourceMassive;
}

[[nodiscard]] inline std::string formatLine(const tick_packet::DecodedTick& tick) {
    const PriceData& price = tick.price;
    const auto tsNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             price.timestamp.time_since_epoch())
                             .count();
    if (isKnownSource(tick.source)) {
        return std::format("{},source={} ask={}i,bid={}i {}\n", price.symbol, tick.source,
                           price.ask, price.bid, tsNanos);
    }
    return std::format("{} ask={}i,bid={}i {}\n", price.symbol, price.ask, price.bid,
                       tsNanos);
}

}  // namespace tick_line
