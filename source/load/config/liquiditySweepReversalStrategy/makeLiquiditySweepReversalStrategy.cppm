// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeLiquiditySweepReversalStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a LiquiditySweepReversalStrategy
// config. Every parameter is read with getInt and no has() fallback: the
// sweep registers every name read here
// (buildLiquiditySweepReversalStrategySweep), so a missing one is a bug that
// should throw at load time, matching the strategy ctor's fail-fast
// validation on the run side.
tradingDefinitions::StrategyConfig makeLiquiditySweepReversalStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    const int pivotBars = combo.getInt("PIVOT_BARS");
    const int lookbackBars = combo.getInt("LOOKBACK_BARS");
    const int validBars = combo.getInt("VALID_BARS");

    // The window must cover the pivot scan (lookback + a left wing beyond its
    // oldest candidate + the in-progress bar) and keep the displacement
    // ATR(10) warm at the oldest valid rejection — the ctor minimum. Derived
    // rather than swept, so every combination is valid by construction — the
    // generator can't express cross-field constraints.
    const int ohlcCount =
        std::max(lookbackBars + pivotBars + 1, validBars + 11);

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in strategyFactory.
            .STRATEGY = "LiquiditySweepReversalStrategy",
            .STOP_DISTANCE_IN_ATR = combo.getInt("STOP_DISTANCE_IN_ATR"),
            .LIMIT_DISTANCE_IN_ATR = combo.getInt("LIMIT_DISTANCE_IN_ATR"),
            .TRADING_SIZE = 1,
        },
        // Positional contract with LiquiditySweepReversalStrategy: [0] is the
        // signal timeframe (it also drives the ATR entry gate).
        .OHLC_VARIABLES = {
            OHLCVariables{
                .OHLC_COUNT = ohlcCount,
                .OHLC_MINUTES = combo.getInt("OHLC_MINUTES"),
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .LIQUIDITY_SWEEP_REVERSAL_VARIABLES = LiquiditySweepReversalVariables{
                .PIVOT_BARS = pivotBars,
                .LOOKBACK_BARS = lookbackBars,
                .MIN_SWEEP_PIPS = combo.getInt("MIN_SWEEP_PIPS"),
                .DISPLACEMENT_ATR_TENTHS = combo.getInt("DISPLACEMENT_ATR_TENTHS"),
                .VALID_BARS = validBars,
                .MAX_TRADE_DURATION_MINUTES =
                    combo.getInt("MAX_TRADE_DURATION_MINUTES"),
            },
        },
    };
}

}  // namespace sweep
