// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module ohlcObject;

import std;  // replaces <chrono>, <cstdint>, <limits>

// One OHLC (open/high/low/close) bar, built from a stream of ticks by the
// ohlcBuilder module. Prices are the same scaled fixed-point INT32 points used
// everywhere in the engine (see priceData / symbolScale) — NOT decimals.
//
// C# analogy: OhlcObject { DateTime date; decimal open/high/low/close; }.
// The defaults mirror the C# ones: high starts at the smallest representable
// value and low at the largest, so the first real price always wins the
// max/min comparisons in the builder.
export struct OhlcObject {
    // Simulation time of the first tick in this bar (C# DateTime.MinValue
    // analogue: a default time_point is the clock's epoch).
    std::chrono::system_clock::time_point date{};

    std::int32_t open  = 0;
    std::int32_t close = 0;
    std::int32_t high  = std::numeric_limits<std::int32_t>::min();
    std::int32_t low   = std::numeric_limits<std::int32_t>::max();

    // Set once the next bar opens — a complete bar's OHLC values are final,
    // the in-progress (last) bar's are still moving with each tick.
    bool complete = false;
};
