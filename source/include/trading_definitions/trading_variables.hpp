// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace trading_definitions {
struct TradingVariables {
    std::string STRATEGY;
    double STOP_DISTANCE_IN_PIPS;
    double LIMIT_DISTANCE_IN_PIPS;
    double TRADING_SIZE;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TradingVariables,
    STRATEGY,
    STOP_DISTANCE_IN_PIPS,
    LIMIT_DISTANCE_IN_PIPS,
    TRADING_SIZE
);
} // namespace strategy
