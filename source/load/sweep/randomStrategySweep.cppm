// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/utilities/parameterSweep.hpp"

export module randomStrategySweep;

export namespace sweep {

// Declares which parameters to sweep for the RandomStrategy. Keeping the ranges
// in one place means a new strategy (or extra swept parameter) is a localised
// edit: register it here, then read it back in makeStrategy().
ParameterGenerator buildRandomStrategySweep() {
    ParameterGenerator generator;
    // generator.addRange("OHLC_COUNT", 80, 20, 140);  // 80, 100, 120, 140
    // generator.addList("OHLC_MINUTES", {1, 3, 5, 8});
    generator.addRange("LIMIT_DISTANCE_IN_PIPS", 1, 1, 100);
    generator.addRange("STOP_DISTANCE_IN_PIPS", 1, 1, 100);
    return generator;
}

}  // namespace sweep
