// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/utilities/parameterSweep.hpp"      // sweep::Combination
#include "shared/tradingDefinitions/strategy.hpp"

export module makeStrategy;

import std;

export namespace sweep {

// Map one swept parameter combination onto a concrete Strategy. Each
// combination gets its own UUID so a single backtest result is uniquely
// identifiable and traceable back to its inputs.
tradingDefinitions::Strategy makeStrategy(const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    return Strategy{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            .STRATEGY = "RandomStrategy",
            .STOP_DISTANCE_IN_PIPS = combo.getInt("STOP_DISTANCE_IN_PIPS"),
            .LIMIT_DISTANCE_IN_PIPS = combo.getInt("LIMIT_DISTANCE_IN_PIPS"),
            .TRADING_SIZE = 1,
        },
        .OHLC_VARIABLES = {
            OHLCVariables{
                // Only read OHLC params when the sweep actually registers them;
                // they default to 0 otherwise (see buildRandomStrategySweep).
                .OHLC_COUNT = combo.has("OHLC_COUNT") ? combo.getInt("OHLC_COUNT") : 0,
                .OHLC_MINUTES = combo.has("OHLC_MINUTES") ? combo.getInt("OHLC_MINUTES") : 0,
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .OHLC_RSI_VARIABLES = OHLCRSIVariables{.RSI_LONG = 60, .RSI_SHORT = 40},
        },
    };
}

}  // namespace sweep
