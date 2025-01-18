// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace strategy {
struct OHLCRSIVariables {
    int RSI_LONG;
    int RSI_SHORT;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OHLCRSIVariables, RSI_LONG, RSI_SHORT);
} // namespace strategy
