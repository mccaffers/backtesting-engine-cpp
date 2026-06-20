// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module exitRules;

import std;        // replaces <cstdint>, <optional>
import trade;      // Trade, Direction
import priceData;  // PriceData

export namespace trading::exit_rules {

// Decide whether the current tick has hit a trade's stop-loss or
// take-profit boundary. Returns the price at which the position would
// close (bid for LONG exits, ask for SHORT exits); `std::nullopt`
// means "no exit on this tick".
//
// SL/TP distances are anchored on the close-side of the entry spread
// (`trade.exitReferencePrice`: the entry-tick bid for LONG, entry-tick
// ask for SHORT) — the price the trade would actually exit at — not
// from the execution price. So a 1-pip stop on a LONG means "exit when
// bid drops 1 pip below the entry bid", which prevents the spread
// itself from triggering an exit on the opening tick.
//
// Distances and the resulting trigger prices are scaled-integer price points
// (a value of 10 on EURUSD, scale 100000, is 10 points == 0.0001 == one classic
// pip). trade.stopPrice / trade.limitPrice are precomputed once in
// TradeManager::openTrade, so this hot-path check is pure integer comparison —
// no decimal arithmetic. Trades on unknown symbols (scale 0) are skipped, and a
// zero distance means that leg is disarmed.
inline std::optional<std::int32_t>
checkExit(const Trade& trade, const PriceData& tick) {
    if (trade.scalingFactor == 0) return std::nullopt;

    if (trade.direction == Direction::LONG) {
        // Exit a long at the bid (the price the broker pays us).
        if (trade.stopDistancePips  != 0 && tick.bid <= trade.stopPrice)  return tick.bid;
        if (trade.limitDistancePips != 0 && tick.bid >= trade.limitPrice) return tick.bid;
    } else {
        // Exit a short at the ask (the price we pay to buy back).
        if (trade.stopDistancePips  != 0 && tick.ask >= trade.stopPrice)  return tick.ask;
        if (trade.limitDistancePips != 0 && tick.ask <= trade.limitPrice) return tick.ask;
    }
    return std::nullopt;
}

} // namespace trading::exit_rules
