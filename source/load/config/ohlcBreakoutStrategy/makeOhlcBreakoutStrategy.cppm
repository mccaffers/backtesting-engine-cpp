// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeOhlcBreakoutStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto an OhlcBreakoutStrategy config.
// Every parameter is read with getInt and no has() fallback: the sweep
// registers all seven names (buildOhlcBreakoutStrategySweep), so a missing one
// is a bug that should throw at load time, matching the strategy ctor's
// fail-fast validation on the run side.
tradingDefinitions::StrategyConfig makeOhlcBreakoutStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in run/operations.cppm.
            .STRATEGY = "OhlcBreakoutStrategy",
            .STOP_DISTANCE_IN_PIPS = combo.getInt("STOP_DISTANCE_IN_PIPS"),
            .LIMIT_DISTANCE_IN_PIPS = combo.getInt("LIMIT_DISTANCE_IN_PIPS"),
            .TRADING_SIZE = 1,
        },
        // Positional contract with OhlcBreakoutStrategy: [0] is the breakout
        // timeframe, [1] the trend timeframe.
        .OHLC_VARIABLES = {
            OHLCVariables{
                .OHLC_COUNT = combo.getInt("BREAKOUT_OHLC_COUNT"),
                .OHLC_MINUTES = combo.getInt("BREAKOUT_OHLC_MINUTES"),
            },
            OHLCVariables{
                .OHLC_COUNT = combo.getInt("TREND_OHLC_COUNT"),
                .OHLC_MINUTES = combo.getInt("TREND_OHLC_MINUTES"),
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .OHLC_BREAKOUT_VARIABLES = OHLCBreakoutVariables{
                .BUFFER_PIPS = combo.getInt("BUFFER_PIPS"),
            },
        },
    };
}

}  // namespace sweep
