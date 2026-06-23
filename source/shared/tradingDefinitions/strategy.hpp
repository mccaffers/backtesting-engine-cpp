// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/variables/tradingVariables.hpp"
#include "shared/tradingDefinitions/variables/ohlcVariables.hpp"
#include "shared/tradingDefinitions/variables/strategyVariables.hpp"

namespace tradingDefinitions {

struct Strategy {
    std::string UUID;
    TradingVariables TRADING_VARIABLES;
    std::vector<OHLCVariables> OHLC_VARIABLES;
    StrategyVariables STRATEGY_VARIABLES;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Strategy,
    UUID,
    TRADING_VARIABLES,
    OHLC_VARIABLES,
    STRATEGY_VARIABLES
);
} 
