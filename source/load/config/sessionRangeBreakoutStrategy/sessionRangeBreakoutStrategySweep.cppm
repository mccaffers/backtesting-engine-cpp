// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module sessionRangeBreakoutStrategySweep;

export import parameterGenerator;  // buildSessionRangeBreakoutStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide. Europe-session symbols only (market_hours' mapping): the strategy
// trades the London open, and with PEAK_HOURS_ONLY on the run loop's entry
// gate only opens for these symbols during exactly that window. Index CFDs
// only since 2026-28: the seven Europe-mapped FX pairs contributed zero of
// the batch's 493 winners — the London-open edge lives on the indices.
inline constexpr std::array kSessionRangeBreakoutSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "DEUIDXEUR", "FRAIDXEUR", "GBRIDXGBP"});

// Declares which parameters to sweep for the SessionRangeBreakoutStrategy.
// The OHLC COUNT is NOT swept: makeSessionRangeBreakoutStrategy derives it
// from OHLC_MINUTES and ENTRY_WINDOW_MINUTES at the ctor minimum (span
// midnight -> entry cutoff), so every combination is valid by construction —
// a deeper window adds nothing because bars are selected by date. Tune the
// values here; makeSessionRangeBreakoutStrategy reads back every name
// registered below.
ParameterGenerator buildSessionRangeBreakoutStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kSessionRangeBreakoutSymbolGroupsOverride>();
    // Signal timeframe the Asian range is built from.
    generator.addList("OHLC_MINUTES", {5, 15, 30, 45});
    // Padding on the Asian high/low, in pips (0 = raw range). The dense
    // {0..5} ladder was flat in 2026-28 — a coarse A/B is enough.
    generator.addList("BUFFER_PIPS", {0, 2, 4});
    // How long after the London open entries may fire. The 2026-28 scores
    // decayed monotonically with the window (180 had volume but the worst
    // scores by far) — the edge is in the first hour, so the grid slides
    // down and 20 probes tighter.
    generator.addList("ENTRY_WINDOW_MINUTES", {20, 30, 40, 60});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock (trades were riding for 10+ hours live). 120 was
    // the pinned edge in 2026-28 — the cap slides up a notch to probe 240.
    generator.addList("MAX_TRADE_DURATION_MINUTES", {60, 120, 240});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped). The "vol-expansion needs a
    // wide stop" hypothesis failed its A/B: 1-ATR stops took 59% of the
    // 2026-28 winners with the best scores, and 3 was the worst — dropped.
    generator.addList("STOP_DISTANCE_IN_ATR", {1, 2});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {3, 5, 7});  // 9 was flat vs 7
    return generator;
}

}  // namespace sweep
