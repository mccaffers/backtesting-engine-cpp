// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <optional>
#include <nlohmann/json.hpp>
#include "ohlc_rsi_variables.hpp"

namespace trading_definitions {

struct StrategyVariables {
    std::optional<OHLCRSIVariables> OHLC_RSI_VARIABLES;
};

void to_json(nlohmann::json& j, const StrategyVariables& s);
void from_json(const nlohmann::json& j, StrategyVariables& s);

}
