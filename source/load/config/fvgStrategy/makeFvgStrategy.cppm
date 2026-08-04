// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeFvgStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto an FvgStrategy config. Every
// parameter is read with getInt and no has() fallback: the sweep registers
// every name read here (buildFvgStrategySweep), so a missing one is a bug
// that should throw at load time, matching the strategy ctor's fail-fast
// validation on the run side.
tradingDefinitions::StrategyConfig makeFvgStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    const int lookbackBars = combo.getInt("LOOKBACK_BARS");
    const int htfSmaPeriod = combo.getInt("HTF_SMA_PERIOD");

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in strategyFactory.
            .STRATEGY = "FvgStrategy",
            .STOP_DISTANCE_IN_ATR = combo.getInt("STOP_DISTANCE_IN_ATR"),
            .LIMIT_DISTANCE_IN_ATR = combo.getInt("LIMIT_DISTANCE_IN_ATR"),
            .TRADING_SIZE = 1,
        },
        // Positional contract with FvgStrategy: [0] is the FVG timeframe,
        // [1] the HTF trend timeframe. The counts are DERIVED at the ctor
        // minimums (LOOKBACK_BARS + 3 / HTF_SMA_PERIOD + 2) rather than
        // swept, so every combination is valid by construction — the
        // generator can't express cross-field constraints.
        .OHLC_VARIABLES = {
            OHLCVariables{
                .OHLC_COUNT = lookbackBars + 3,
                .OHLC_MINUTES = combo.getInt("FVG_OHLC_MINUTES"),
            },
            OHLCVariables{
                .OHLC_COUNT = htfSmaPeriod + 2,
                .OHLC_MINUTES = combo.getInt("HTF_OHLC_MINUTES"),
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .FVG_STRATEGY_VARIABLES = FVGStrategyVariables{
                .LOOKBACK_BARS = lookbackBars,
                .MIN_GAP_PIPS = combo.getInt("MIN_GAP_PIPS"),
                .HTF_SMA_PERIOD = htfSmaPeriod,
                .MIN_GAP_AGE_BARS = combo.getInt("MIN_GAP_AGE_BARS"),
                .MAX_TRADE_DURATION_MINUTES =
                    combo.getInt("MAX_TRADE_DURATION_MINUTES"),
            },
        },
    };
}

}  // namespace sweep
