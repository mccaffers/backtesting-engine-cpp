// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module randomStrategySweep;

export import parameterGenerator;  // buildRandomStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against. Leave empty to run the full
// default set (sweep::kSymbolGroups in runConfigurationBuilder); list groups
// here to narrow the sweep, e.g.
//   std::to_array<std::string_view>({"EURUSD", "GBPUSD"})   // two runs
//   std::to_array<std::string_view>({"EURUSD,GBPUSD"})      // one run, both instruments
// setSymbolGroups validates every symbol against symbol_scale::kTable at
// compile time, so a typo here fails the build instead of queueing a run the
// worker rejects at runtime.
inline constexpr std::array kSymbolGroupsOverride =
    std::to_array<std::string_view>({"EURUSD"});

// Declares which parameters to sweep for the RandomStrategy. Keeping the ranges
// in one place means a new strategy (or extra swept parameter) is a localised
// edit: register it here, then read it back in makeStrategy().
ParameterGenerator buildRandomStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kSymbolGroupsOverride>();
    // generator.addRange("OHLC_COUNT", 80, 20, 140);  // 80, 100, 120, 140
    // generator.addList("OHLC_MINUTES", {1, 3, 5, 8});
    generator.addRange("LIMIT_DISTANCE_IN_PIPS", 1, 1, 100);
    generator.addRange("STOP_DISTANCE_IN_PIPS", 1, 1, 100);
    return generator;
}

}  // namespace sweep
