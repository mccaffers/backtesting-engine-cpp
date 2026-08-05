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
    if (s.FVG_STRATEGY_VARIABLES) {
        j["FVG_STRATEGY_VARIABLES"] = *s.FVG_STRATEGY_VARIABLES;
    } else {
        j["FVG_STRATEGY_VARIABLES"] = nullptr;
    }
    if (s.KELTNER_FADE_VARIABLES) {
        j["KELTNER_FADE_VARIABLES"] = *s.KELTNER_FADE_VARIABLES;
    } else {
        j["KELTNER_FADE_VARIABLES"] = nullptr;
    }
    if (s.SESSION_RANGE_BREAKOUT_VARIABLES) {
        j["SESSION_RANGE_BREAKOUT_VARIABLES"] = *s.SESSION_RANGE_BREAKOUT_VARIABLES;
    } else {
        j["SESSION_RANGE_BREAKOUT_VARIABLES"] = nullptr;
    }
    if (s.SQUEEZE_BREAKOUT_VARIABLES) {
        j["SQUEEZE_BREAKOUT_VARIABLES"] = *s.SQUEEZE_BREAKOUT_VARIABLES;
    } else {
        j["SQUEEZE_BREAKOUT_VARIABLES"] = nullptr;
    }
    if (s.NY_OPEN_RANGE_BREAKOUT_VARIABLES) {
        j["NY_OPEN_RANGE_BREAKOUT_VARIABLES"] = *s.NY_OPEN_RANGE_BREAKOUT_VARIABLES;
    } else {
        j["NY_OPEN_RANGE_BREAKOUT_VARIABLES"] = nullptr;
    }
    if (s.LIQUIDITY_SWEEP_REVERSAL_VARIABLES) {
        j["LIQUIDITY_SWEEP_REVERSAL_VARIABLES"] = *s.LIQUIDITY_SWEEP_REVERSAL_VARIABLES;
    } else {
        j["LIQUIDITY_SWEEP_REVERSAL_VARIABLES"] = nullptr;
    }
    if (s.RANGE_VELOCITY_VARIABLES) {
        j["RANGE_VELOCITY_VARIABLES"] = *s.RANGE_VELOCITY_VARIABLES;
    } else {
        j["RANGE_VELOCITY_VARIABLES"] = nullptr;
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
    if (j.contains("FVG_STRATEGY_VARIABLES") && !j.at("FVG_STRATEGY_VARIABLES").is_null()) {
        s.FVG_STRATEGY_VARIABLES = j.at("FVG_STRATEGY_VARIABLES").get<FVGStrategyVariables>();
    } else {
        s.FVG_STRATEGY_VARIABLES = std::nullopt;
    }
    if (j.contains("KELTNER_FADE_VARIABLES") && !j.at("KELTNER_FADE_VARIABLES").is_null()) {
        s.KELTNER_FADE_VARIABLES = j.at("KELTNER_FADE_VARIABLES").get<KeltnerFadeVariables>();
    } else {
        s.KELTNER_FADE_VARIABLES = std::nullopt;
    }
    if (j.contains("SESSION_RANGE_BREAKOUT_VARIABLES") &&
        !j.at("SESSION_RANGE_BREAKOUT_VARIABLES").is_null()) {
        s.SESSION_RANGE_BREAKOUT_VARIABLES =
            j.at("SESSION_RANGE_BREAKOUT_VARIABLES").get<SessionRangeBreakoutVariables>();
    } else {
        s.SESSION_RANGE_BREAKOUT_VARIABLES = std::nullopt;
    }
    if (j.contains("SQUEEZE_BREAKOUT_VARIABLES") &&
        !j.at("SQUEEZE_BREAKOUT_VARIABLES").is_null()) {
        s.SQUEEZE_BREAKOUT_VARIABLES =
            j.at("SQUEEZE_BREAKOUT_VARIABLES").get<SqueezeBreakoutVariables>();
    } else {
        s.SQUEEZE_BREAKOUT_VARIABLES = std::nullopt;
    }
    if (j.contains("NY_OPEN_RANGE_BREAKOUT_VARIABLES") &&
        !j.at("NY_OPEN_RANGE_BREAKOUT_VARIABLES").is_null()) {
        s.NY_OPEN_RANGE_BREAKOUT_VARIABLES =
            j.at("NY_OPEN_RANGE_BREAKOUT_VARIABLES").get<NyOpenRangeBreakoutVariables>();
    } else {
        s.NY_OPEN_RANGE_BREAKOUT_VARIABLES = std::nullopt;
    }
    if (j.contains("LIQUIDITY_SWEEP_REVERSAL_VARIABLES") &&
        !j.at("LIQUIDITY_SWEEP_REVERSAL_VARIABLES").is_null()) {
        s.LIQUIDITY_SWEEP_REVERSAL_VARIABLES =
            j.at("LIQUIDITY_SWEEP_REVERSAL_VARIABLES").get<LiquiditySweepReversalVariables>();
    } else {
        s.LIQUIDITY_SWEEP_REVERSAL_VARIABLES = std::nullopt;
    }
    if (j.contains("RANGE_VELOCITY_VARIABLES") &&
        !j.at("RANGE_VELOCITY_VARIABLES").is_null()) {
        s.RANGE_VELOCITY_VARIABLES =
            j.at("RANGE_VELOCITY_VARIABLES").get<RangeVelocityVariables>();
    } else {
        s.RANGE_VELOCITY_VARIABLES = std::nullopt;
    }
}

}
