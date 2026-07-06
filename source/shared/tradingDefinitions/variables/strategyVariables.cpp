// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#include "shared/tradingDefinitions/variables/strategyVariables.hpp"

namespace tradingDefinitions {

void to_json(nlohmann::json& j, const StrategyVariables& s) {
    if (s.OHLC_RSI_VARIABLES) {
        j["OHLC_RSI_VARIABLES"] = *s.OHLC_RSI_VARIABLES;
    } else {
        j["OHLC_RSI_VARIABLES"] = nullptr;
    }
    if (s.OHLC_BREAKOUT_VARIABLES) {
        j["OHLC_BREAKOUT_VARIABLES"] = *s.OHLC_BREAKOUT_VARIABLES;
    } else {
        j["OHLC_BREAKOUT_VARIABLES"] = nullptr;
    }
}

void from_json(const nlohmann::json& j, StrategyVariables& s) {
    if (j.contains("OHLC_RSI_VARIABLES") && !j.at("OHLC_RSI_VARIABLES").is_null()) {
        s.OHLC_RSI_VARIABLES = j.at("OHLC_RSI_VARIABLES").get<OHLCRSIVariables>();
    } else {
        s.OHLC_RSI_VARIABLES = std::nullopt;
    }
    if (j.contains("OHLC_BREAKOUT_VARIABLES") && !j.at("OHLC_BREAKOUT_VARIABLES").is_null()) {
        s.OHLC_BREAKOUT_VARIABLES = j.at("OHLC_BREAKOUT_VARIABLES").get<OHLCBreakoutVariables>();
    } else {
        s.OHLC_BREAKOUT_VARIABLES = std::nullopt;
    }
}

}
