// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <optional>
#include <boost/decimal.hpp>
#include "models/trade.hpp"
#include "models/priceData.hpp"

namespace trading::exit_rules {

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
// Pip → price conversion uses the symbol's scaling factor: a 1.5-pip
// distance on EURUSD (scale 10000) is 0.00015; on USDJPY (scale 100)
// it's 0.015. Trades on unknown symbols (scale 0) are skipped — there
// is no sensible pip distance to apply.
inline std::optional<boost::decimal::decimal64_t>
checkExit(const Trade& trade, const PriceData& tick) {
    if (trade.scalingFactor == 0) return std::nullopt;
    if (trade.stopDistancePips == 0 &&
        trade.limitDistancePips == 0) {
        return std::nullopt;
    }

    const auto stopOffset  = trade.stopDistancePips  / trade.scalingFactor;
    const auto limitOffset = trade.limitDistancePips / trade.scalingFactor;

    if (trade.direction == Direction::LONG) {
        const auto stopPrice  = trade.exitReferencePrice - stopOffset;
        const auto limitPrice = trade.exitReferencePrice + limitOffset;
        // Exit a long at the bid (the price the broker pays us).
        if (trade.stopDistancePips  != 0 && tick.bid <= stopPrice)  return tick.bid;
        if (trade.limitDistancePips != 0 && tick.bid >= limitPrice) return tick.bid;
    } else {
        const auto stopPrice  = trade.exitReferencePrice + stopOffset;
        const auto limitPrice = trade.exitReferencePrice - limitOffset;
        // Exit a short at the ask (the price we pay to buy back).
        if (trade.stopDistancePips  != 0 && tick.ask >= stopPrice)  return tick.ask;
        if (trade.limitDistancePips != 0 && tick.ask <= limitPrice) return tick.ask;
    }
    return std::nullopt;
}

} // namespace trading::exit_rules
