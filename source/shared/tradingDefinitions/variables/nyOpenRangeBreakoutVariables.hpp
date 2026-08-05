// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the New York open-range breakout strategy. RANGE_HOURS is
// the depth of the pre-open range, in whole hours counted back from the NY
// equities open (13:30 UTC under EDT, else 14:30) — 13 approximates the full
// overnight session, 4 a tight pre-open coil; it is swept rather than
// hardcoded so the data decides which range definition carries the edge.
// BUFFER_PIPS pads the range high/low breakout levels (converted to integer
// points via symbol_scale::get) — a noise filter against marginal pokes.
// ENTRY_WINDOW_MINUTES bounds how long after the open entries may fire; the
// NewYork peak-hours session is three hours from the open, so values above
// 180 buy nothing when PEAK_HOURS_ONLY is on. MAX_TRADE_DURATION_MINUTES caps
// a trade's lifetime exactly like the session range breakout's: during()
// closes the symbol's trade once it has been open STRICTLY longer than this;
// <= 0 disables the cap. WITH_DEFAULT so winner configs persisted before a
// field existed parse with the in-class defaults instead of throwing; the
// strategy ctor rejects zero RANGE_HOURS / ENTRY_WINDOW_MINUTES loudly, so an
// absent required field still fails fast — at construction, not at parse.
struct NyOpenRangeBreakoutVariables {
    int RANGE_HOURS = 0;
    int BUFFER_PIPS = 0;
    int ENTRY_WINDOW_MINUTES = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NyOpenRangeBreakoutVariables,
                                                RANGE_HOURS,
                                                BUFFER_PIPS,
                                                ENTRY_WINDOW_MINUTES,
                                                MAX_TRADE_DURATION_MINUTES);
}
