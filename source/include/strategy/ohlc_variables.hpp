// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace strategy {
struct OHLCVariables {
    int OHLC_COUNT;
    int OHLC_MINUTES;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OHLCVariables, OHLC_COUNT, OHLC_MINUTES);
} // namespace strategy
