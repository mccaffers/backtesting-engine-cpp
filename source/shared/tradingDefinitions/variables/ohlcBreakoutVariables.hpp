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
struct OHLCBreakoutVariables {
    int BUFFER_PIPS = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OHLCBreakoutVariables, BUFFER_PIPS);
}
