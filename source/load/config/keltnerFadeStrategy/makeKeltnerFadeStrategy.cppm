// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module makeKeltnerFadeStrategy;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a KeltnerFadeStrategy config.
// Every parameter is read with getInt and no has() fallback: the sweep
// registers every name read here (buildKeltnerFadeStrategySweep), so a
// missing one is a bug that should throw at load time, matching the strategy
// ctor's fail-fast validation on the run side.
tradingDefinitions::StrategyConfig makeKeltnerFadeStrategy(
    const sweep::Combination& combo) {
    using namespace tradingDefinitions;

    const int bandSmaPeriod = combo.getInt("BAND_SMA_PERIOD");

    return StrategyConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            // Must match the dispatch string in strategyFactory.
            .STRATEGY = "KeltnerFadeStrategy",
            .STOP_DISTANCE_IN_ATR = combo.getInt("STOP_DISTANCE_IN_ATR"),
            .LIMIT_DISTANCE_IN_ATR = combo.getInt("LIMIT_DISTANCE_IN_ATR"),
            .TRADING_SIZE = 1,
        },
        // Positional contract with KeltnerFadeStrategy: [0] is the signal
        // timeframe (it also drives the ATR entry gate). The count is DERIVED
        // at the ctor minimum (BAND_SMA_PERIOD + 2) rather than swept, so
        // every combination is valid by construction — the generator can't
        // express cross-field constraints.
        .OHLC_VARIABLES = {
            OHLCVariables{
                .OHLC_COUNT = bandSmaPeriod + 2,
                .OHLC_MINUTES = combo.getInt("OHLC_MINUTES"),
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .KELTNER_FADE_VARIABLES = KeltnerFadeVariables{
                .BAND_SMA_PERIOD = bandSmaPeriod,
                .BAND_ATR_MULT_TENTHS = combo.getInt("BAND_ATR_MULT_TENTHS"),
                .MAX_TRADE_DURATION_MINUTES =
                    combo.getInt("MAX_TRADE_DURATION_MINUTES"),
            },
        },
    };
}

}  // namespace sweep
