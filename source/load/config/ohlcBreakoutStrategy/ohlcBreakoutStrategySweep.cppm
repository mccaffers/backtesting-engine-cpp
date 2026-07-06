// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module ohlcBreakoutStrategySweep;

export import parameterGenerator;  // buildOhlcBreakoutStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide.
inline constexpr std::array kOhlcBreakoutSymbolGroupsOverride =
    std::to_array<std::string_view>({"EURUSD"});

// Declares which parameters to sweep for the OhlcBreakoutStrategy — all seven.
// Seven dimensions multiply fast: this set expands to 40^4 x 6 x 12 x 12
// ≈ 2.2 BILLION combinations per run (loadCommand shows the exact count and
// asks for confirmation before queueing). Tune the values here;
// makeOhlcBreakoutStrategy reads back every name registered below, and the
// strategy ctor requires OHLC_COUNT >= 2 and OHLC_MINUTES >= 1.
ParameterGenerator buildOhlcBreakoutStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kOhlcBreakoutSymbolGroupsOverride>();
    // Breakout timeframe: the closed-candle range price must clear.
    generator.addRange("BREAKOUT_OHLC_MINUTES", 20, 20, 120);
    generator.addRange("BREAKOUT_OHLC_COUNT", 20, 20, 120);
    // Trend timeframe: EMA over its closes (period = count / 2) is the filter.
    generator.addRange("TREND_OHLC_MINUTES", 20, 20, 120);
    generator.addRange("TREND_OHLC_COUNT", 20, 20, 120);
    // Padding on the breakout levels, in pips (0 = raw range).
    generator.addRange("BUFFER_PIPS", 0, 2, 10);
    // Exits are central (Operations enforces SL/TP), so the distances sweep too.
    generator.addRange("STOP_DISTANCE_IN_PIPS", 10, 20, 110);
    generator.addRange("LIMIT_DISTANCE_IN_PIPS", 10, 20, 110);
    return generator;
}

}  // namespace sweep
