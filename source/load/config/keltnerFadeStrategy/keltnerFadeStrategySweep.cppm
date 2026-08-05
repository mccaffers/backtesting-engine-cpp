// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module keltnerFadeStrategySweep;

export import parameterGenerator;  // buildKeltnerFadeStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide. Mean reversion targets the ranging crosses; EURUSD rides along as
// the trending-major control the hypothesis should do WORSE on.
inline constexpr std::array kKeltnerFadeSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "EURGBP", "EURCHF", "AUDNZD", "USDCHF", "EURNOK", "USDSEK", "EURUSD"});

// Declares which parameters to sweep for the KeltnerFadeStrategy. The OHLC
// COUNT is NOT swept: makeKeltnerFadeStrategy derives it at the ctor minimum
// (BAND_SMA_PERIOD + 2), so every combination is valid by construction — a
// deeper window adds nothing because the band only reads BAND_SMA_PERIOD + 1
// closed bars. Tune the values here; makeKeltnerFadeStrategy reads back every
// name registered below.
ParameterGenerator buildKeltnerFadeStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kKeltnerFadeSymbolGroupsOverride>();
    // Signal timeframe: band centre + width both live on it.
    generator.addList("OHLC_MINUTES", {5, 15, 30});
    // Lookback for the band centre (SMA) and width (ATR over the same bars).
    generator.addList("BAND_SMA_PERIOD", {20, 50});
    // Band half-width in TENTHS of an ATR (15 = 1.5x): how stretched price
    // must be before it is faded. 40 probes the extreme-stretch tail — the
    // regime where the reversion thesis is strongest and the grid had no
    // coverage.
    generator.addList("BAND_ATR_MULT_TENTHS", {15, 20, 25, 30, 40});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this — the fade's thesis clock (a stretch that
    // has not snapped back in time is a failed reversion). No uncapped
    // variant — every winner must carry an exit clock.
    generator.addList("MAX_TRADE_DURATION_MINUTES", {30, 60, 120});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped). Reversion inverts the trend
    // strategies' shape — the limit sits NEARER than the stop (take the
    // snap-back, survive the excursion).
    generator.addList("STOP_DISTANCE_IN_ATR", {2, 3});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {1, 2, 3});
    return generator;
}

}  // namespace sweep
