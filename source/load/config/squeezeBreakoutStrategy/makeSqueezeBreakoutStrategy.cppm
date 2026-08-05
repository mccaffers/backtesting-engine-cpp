// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeSqueezeBreakoutStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a SqueezeBreakoutStrategy config.
// Every parameter is read with getInt and no has() fallback: the sweep
// registers every name read here (buildSqueezeBreakoutStrategySweep), so a
// missing one is a bug that should throw at load time, matching the strategy
// ctor's fail-fast validation on the run side.
tradingDefinitions::StrategyConfig makeSqueezeBreakoutStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    const int nrLookback = combo.getInt("NR_LOOKBACK");
    const int validBars = combo.getInt("VALID_BARS");

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in strategyFactory.
            .STRATEGY = "SqueezeBreakoutStrategy",
            .STOP_DISTANCE_IN_ATR = combo.getInt("STOP_DISTANCE_IN_ATR"),
            .LIMIT_DISTANCE_IN_ATR = combo.getInt("LIMIT_DISTANCE_IN_ATR"),
            .TRADING_SIZE = 1,
        },
        // Positional contract with SqueezeBreakoutStrategy: [0] is the signal
        // timeframe (it also drives the ATR entry gate), [1] the trend
        // timeframe. The signal count is DERIVED at the ctor minimum
        // (VALID_BARS + max(1, NR_LOOKBACK - 1) + 1) rather than swept, so
        // every combination is valid by construction — the generator can't
        // express cross-field constraints.
        .OHLC_VARIABLES = {
            OHLCVariables{
                .OHLC_COUNT = validBars + std::max(1, nrLookback - 1) + 1,
                .OHLC_MINUTES = combo.getInt("OHLC_MINUTES"),
            },
            OHLCVariables{
                .OHLC_COUNT = combo.getInt("TREND_OHLC_COUNT"),
                .OHLC_MINUTES = combo.getInt("TREND_OHLC_MINUTES"),
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .SQUEEZE_BREAKOUT_VARIABLES = SqueezeBreakoutVariables{
                .NR_LOOKBACK = nrLookback,
                .VALID_BARS = validBars,
                .BUFFER_PIPS = combo.getInt("BUFFER_PIPS"),
                .MAX_TRADE_DURATION_MINUTES =
                    combo.getInt("MAX_TRADE_DURATION_MINUTES"),
            },
        },
    };
}

}  // namespace sweep
