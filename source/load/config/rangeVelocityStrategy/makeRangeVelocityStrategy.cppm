// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeRangeVelocityStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a RangeVelocityStrategy config.
// Every parameter is read with getInt and no has() fallback: the sweep
// registers every name read here (buildRangeVelocityStrategySweep), so a
// missing one is a bug that should throw at load time, matching the strategy
// ctor's fail-fast validation on the run side.
tradingDefinitions::StrategyConfig makeRangeVelocityStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    const int runBars = combo.getInt("RUN_BARS");
    const int speedLookbackBars = combo.getInt("SPEED_LOOKBACK_BARS");
    const int exitRunBars = combo.getInt("EXIT_RUN_BARS");

    // The bar window must cover the run + speed baseline scan and the
    // during() opposite-run scan — the ctor minimum, +2 margin (the count
    // also drives the live QuestDB warm-up depth, and reading from the end
    // makes extra depth harmless). Derived rather than swept, so every
    // combination is valid by construction — the generator can't express
    // cross-field constraints (the OHLC_COUNT doctrine).
    const int rangeCount =
        std::max(runBars + speedLookbackBars, exitRunBars) + 2;

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in strategyFactory.
            .STRATEGY = "RangeVelocityStrategy",
            .STOP_DISTANCE_IN_ATR = combo.getInt("STOP_DISTANCE_IN_ATR"),
            .LIMIT_DISTANCE_IN_ATR = combo.getInt("LIMIT_DISTANCE_IN_ATR"),
            .TRADING_SIZE = 1,
        },
        // Deliberately no OHLC series: the strategy trades range bars only,
        // and the ATR entry gate falls back to its default 15m series for
        // stop/limit sizing (entryConditions::gateSeriesFor, the
        // RandomStrategy path).
        .OHLC_VARIABLES = {},
        // Positional contract with RangeVelocityStrategy: [0] is THE series
        // it trades.
        .RANGE_VARIABLES = {
            RangeBarVariables{
                .RANGE_ATR_TICK_WINDOW = combo.getInt("RANGE_ATR_TICK_WINDOW"),
                .RANGE_ATR_PERCENT = combo.getInt("RANGE_ATR_PERCENT"),
                .RANGE_COUNT = rangeCount,
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .RANGE_VELOCITY_VARIABLES = RangeVelocityVariables{
                .RUN_BARS = runBars,
                .SPEED_LOOKBACK_BARS = speedLookbackBars,
                .SPEED_RATIO_PERCENT = combo.getInt("SPEED_RATIO_PERCENT"),
                .EXIT_RUN_BARS = exitRunBars,
                .MAX_TRADE_DURATION_MINUTES =
                    combo.getInt("MAX_TRADE_DURATION_MINUTES"),
            },
        },
    };
}

}  // namespace sweep
