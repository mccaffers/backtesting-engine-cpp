// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the OHLC breakout strategy. BUFFER_PIPS pads the
// closed-candle high/low breakout levels: price must clear the level by this
// many pips (converted to integer points via symbol_scale::get) before a
// signal fires — a noise filter against marginal pokes through the range.
// MAX_TRADE_DURATION_MINUTES caps a trade's lifetime: during() closes the
// symbol's trade once it has been open STRICTLY longer than this; <= 0
// disables the cap. WITH_DEFAULT so winner configs persisted before the
// field existed parse with the in-class defaults (absent = 0 = disabled)
// instead of throwing — which also makes BUFFER_PIPS tolerant of absence.
struct OHLCBreakoutVariables {
    int BUFFER_PIPS = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OHLCBreakoutVariables,
                                                BUFFER_PIPS,
                                                MAX_TRADE_DURATION_MINUTES);
}
