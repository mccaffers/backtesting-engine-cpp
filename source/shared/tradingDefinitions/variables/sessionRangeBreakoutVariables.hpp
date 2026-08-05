// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the session open-range breakout strategy. BUFFER_PIPS pads
// the Asian-session high/low breakout levels (converted to integer points via
// symbol_scale::get) — a noise filter against marginal pokes through the
// range. ENTRY_WINDOW_MINUTES bounds how long after the London open entries
// may fire: the open-range edge decays through the session, so the window is
// a first-class swept parameter. MAX_TRADE_DURATION_MINUTES caps a trade's
// lifetime exactly like the OHLC breakout's: during() closes the symbol's
// trade once it has been open STRICTLY longer than this; <= 0 disables the
// cap (the session strategy uses it to avoid riding a London entry into New
// York chop). WITH_DEFAULT so winner configs persisted before a field existed
// parse with the in-class defaults instead of throwing; the strategy ctor
// rejects a zero ENTRY_WINDOW_MINUTES loudly, so an absent required field
// still fails fast — at construction, not at parse.
struct SessionRangeBreakoutVariables {
    int BUFFER_PIPS = 0;
    int ENTRY_WINDOW_MINUTES = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SessionRangeBreakoutVariables,
                                                BUFFER_PIPS,
                                                ENTRY_WINDOW_MINUTES,
                                                MAX_TRADE_DURATION_MINUTES);
}
