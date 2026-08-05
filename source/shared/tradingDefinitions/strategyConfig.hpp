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
#include "shared/tradingDefinitions/variables/rangeBarVariables.hpp"
#include "shared/tradingDefinitions/variables/strategyVariables.hpp"

namespace tradingDefinitions {

struct StrategyConfig {
    std::string UUID;
    TradingVariables TRADING_VARIABLES;
    std::vector<OHLCVariables> OHLC_VARIABLES;
    std::vector<RangeBarVariables> RANGE_VARIABLES;
    StrategyVariables STRATEGY_VARIABLES;
};

// Hand-written (rather than NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE) so the
// original fields stay strictly required while RANGE_VARIABLES falls back to
// empty — every ES winner document, queued Redis payload and test fixture
// written before range bars existed still parses (the macro's j.at would
// reject them all, and liveWinners' per-hit guard would then silently drop
// every winner). Same doctrine as RunConfiguration's serializers.
inline void to_json(nlohmann::json& j, const StrategyConfig& c) {
    j = nlohmann::json{
        {"UUID", c.UUID},
        {"TRADING_VARIABLES", c.TRADING_VARIABLES},
        {"OHLC_VARIABLES", c.OHLC_VARIABLES},
        {"RANGE_VARIABLES", c.RANGE_VARIABLES},
        {"STRATEGY_VARIABLES", c.STRATEGY_VARIABLES},
    };
}

inline void from_json(const nlohmann::json& j, StrategyConfig& c) {
    j.at("UUID").get_to(c.UUID);
    j.at("TRADING_VARIABLES").get_to(c.TRADING_VARIABLES);
    j.at("OHLC_VARIABLES").get_to(c.OHLC_VARIABLES);
    c.RANGE_VARIABLES =
        j.value("RANGE_VARIABLES", std::vector<RangeBarVariables>{});
    j.at("STRATEGY_VARIABLES").get_to(c.STRATEGY_VARIABLES);
}
}
