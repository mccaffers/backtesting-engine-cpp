// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// ohlcBuilder — incremental tick -> OHLC bar aggregation.
//
// Port of the C# CalculateOHLC(PriceObj, decimal price, TimeSpan duration,
// List<OhlcObject>) helper. Called once per tick, it appends to / updates a
// caller-owned vector of bars: the last element is always the in-progress bar,
// everything before it is complete. Shared so both the backtester (run) and a
// future live path can build bars the same way.
//
// Type mapping from the C# original:
//  - decimal price          -> std::int32_t scaled points (engine convention;
//                              the caller picks ask/bid/mid, exactly as the C#
//                              passed price separately from priceObj)
//  - TimeSpan duration      -> std::chrono::system_clock::duration
//                              (std::chrono::minutes etc. convert implicitly)
//  - List<OhlcObject>       -> std::vector<OhlcObject>&, mutated in place
//                              (the C# returned the same list it mutated)

export module ohlcBuilder;

import std;         // replaces <chrono>, <cstdint>, <vector>
import priceData;   // PriceData tick (timestamp source)
import ohlcObject;  // the bar record being built

export namespace ohlc {

void calculateOHLC(const PriceData& tick, std::int32_t price,
                   std::chrono::system_clock::duration duration,
                   std::vector<OhlcObject>& bars) {
    // First tick ever: seed the initial bar from it.
    if (bars.empty()) {
        bars.push_back({.date = tick.timestamp,
                        .open = price,
                        .close = price,
                        .high = price,
                        .low = price});
    }

    // Strictly greater-than, matching the C# `diff > duration.TotalMinutes`:
    // a tick landing exactly on the boundary still belongs to the open bar.
    if (tick.timestamp - bars.back().date > duration) {
        // C# behaviour, ported as-is: the completed bar's close is overwritten
        // with the first price of the NEXT bucket, so consecutive bars join up.
        bars.back().complete = true;
        bars.back().close = price;

        bars.push_back({.date = tick.timestamp,
                        .open = price,
                        .close = price,
                        .high = price,
                        .low = price});
    }

    OhlcObject& bar = bars.back();
    if (price > bar.high) {
        bar.high = price;
    }
    if (price < bar.low) {
        bar.low = price;
    }
    bar.close = price;
}

}  // namespace ohlc
