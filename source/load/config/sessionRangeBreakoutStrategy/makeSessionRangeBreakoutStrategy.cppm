// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeSessionRangeBreakoutStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a SessionRangeBreakoutStrategy
// config. Every parameter is read with getInt and no has() fallback: the
// sweep registers every name read here
// (buildSessionRangeBreakoutStrategySweep), so a missing one is a bug that
// should throw at load time, matching the strategy ctor's fail-fast
// validation on the run side.
tradingDefinitions::StrategyConfig makeSessionRangeBreakoutStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    const int ohlcMinutes = combo.getInt("OHLC_MINUTES");
    const int entryWindowMinutes = combo.getInt("ENTRY_WINDOW_MINUTES");

    // The window must span midnight through the entry cutoff on a winter day
    // (08:00 London open = 480 minutes), with the ctor's two-bar margin:
    // ceil((480 + window) / minutes) + 2 bars. Derived rather than swept, so
    // every combination is valid by construction — the generator can't
    // express cross-field constraints.
    const int ohlcCount =
        (480 + entryWindowMinutes + ohlcMinutes - 1) / ohlcMinutes + 2;

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in strategyFactory.
            .STRATEGY = "SessionRangeBreakoutStrategy",
            .STOP_DISTANCE_IN_ATR = combo.getInt("STOP_DISTANCE_IN_ATR"),
            .LIMIT_DISTANCE_IN_ATR = combo.getInt("LIMIT_DISTANCE_IN_ATR"),
            .TRADING_SIZE = 1,
        },
        // Positional contract with SessionRangeBreakoutStrategy: [0] is the
        // signal timeframe (it also drives the ATR entry gate).
        .OHLC_VARIABLES = {
            OHLCVariables{
                .OHLC_COUNT = ohlcCount,
                .OHLC_MINUTES = ohlcMinutes,
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .SESSION_RANGE_BREAKOUT_VARIABLES = SessionRangeBreakoutVariables{
                .BUFFER_PIPS = combo.getInt("BUFFER_PIPS"),
                .ENTRY_WINDOW_MINUTES = entryWindowMinutes,
                .MAX_TRADE_DURATION_MINUTES =
                    combo.getInt("MAX_TRADE_DURATION_MINUTES"),
            },
        },
    };
}

}  // namespace sweep
