// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "randomStrategySweep.hpp"

namespace sweep {

ParameterGenerator buildRandomStrategySweep() {
    ParameterGenerator generator;
    // generator.addRange("OHLC_COUNT", 80, 20, 140);  // 80, 100, 120, 140
    // generator.addList("OHLC_MINUTES", {1, 3, 5, 8});
    generator.addList("STOP_DISTANCE_IN_PIPS", {1.0, 1.5, 10.0});
    generator.addList("LIMIT_DISTANCE_IN_PIPS", {1.0, 1.5, 10.0});
    return generator;
}

}  // namespace sweep
