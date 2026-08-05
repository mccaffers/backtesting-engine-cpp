// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <optional>
#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/variables/ohlcRsiVariables.hpp"
#include "shared/tradingDefinitions/variables/ohlcBreakoutVariables.hpp"
#include "shared/tradingDefinitions/variables/fvgVariables.hpp"
#include "shared/tradingDefinitions/variables/keltnerFadeVariables.hpp"
#include "shared/tradingDefinitions/variables/sessionRangeBreakoutVariables.hpp"
#include "shared/tradingDefinitions/variables/squeezeBreakoutVariables.hpp"
#include "shared/tradingDefinitions/variables/nyOpenRangeBreakoutVariables.hpp"
#include "shared/tradingDefinitions/variables/liquiditySweepReversalVariables.hpp"
#include "shared/tradingDefinitions/variables/rangeVelocityVariables.hpp"

namespace tradingDefinitions {

// Add in custom parameters
struct StrategyVariables {
    std::optional<OHLCRSIVariables> OHLC_RSI_VARIABLES;
    std::optional<OHLCBreakoutVariables> OHLC_BREAKOUT_VARIABLES;
    std::optional<FVGStrategyVariables> FVG_STRATEGY_VARIABLES;
    std::optional<KeltnerFadeVariables> KELTNER_FADE_VARIABLES;
    std::optional<SessionRangeBreakoutVariables> SESSION_RANGE_BREAKOUT_VARIABLES;
    std::optional<SqueezeBreakoutVariables> SQUEEZE_BREAKOUT_VARIABLES;
    std::optional<NyOpenRangeBreakoutVariables> NY_OPEN_RANGE_BREAKOUT_VARIABLES;
    std::optional<LiquiditySweepReversalVariables> LIQUIDITY_SWEEP_REVERSAL_VARIABLES;
    std::optional<RangeVelocityVariables> RANGE_VELOCITY_VARIABLES;
};

void to_json(nlohmann::json& j, const StrategyVariables& s);
void from_json(const nlohmann::json& j, StrategyVariables& s);

}
