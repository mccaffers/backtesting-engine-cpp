// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <boost/decimal.hpp>
#include <nlohmann/json.hpp>
#include "utilities/decimal_json.hpp"

namespace trading_definitions {
struct TradingVariables {
    std::string STRATEGY;
    boost::decimal::decimal64_t STOP_DISTANCE_IN_PIPS;
    boost::decimal::decimal64_t LIMIT_DISTANCE_IN_PIPS;
    boost::decimal::decimal64_t TRADING_SIZE;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TradingVariables,
    STRATEGY,
    STOP_DISTANCE_IN_PIPS,
    LIMIT_DISTANCE_IN_PIPS,
    TRADING_SIZE
);
}
