// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module nyOpenRangeBreakoutStrategySweep;

export import parameterGenerator;  // buildNyOpenRangeBreakoutStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide. NewYork-session symbols only (market_hours' mapping): the
// strategy trades the NY open, and with PEAK_HOURS_ONLY on the run loop's
// entry gate only opens for these symbols during exactly that window.
// PROBE-OR-RETIRE grid since 2026-28: 56 winners from ~23k runs and a best
// score of 32 that barely clears live selection. This minimal grid keeps
// only the three symbols that produced any winner at all (USA30, LIGHT and
// USDCAD produced none) and the parameter cells the winners actually used —
// if the next batches stay this thin, retire it like KeltnerFade.
inline constexpr std::array kNyOpenRangeBreakoutSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "USA500IDXUSD", "USATECHIDXUSD", "XAUUSD"});

// Declares which parameters to sweep for the NyOpenRangeBreakoutStrategy.
// The OHLC COUNT is NOT swept: makeNyOpenRangeBreakoutStrategy derives it
// from RANGE_HOURS, OHLC_MINUTES and ENTRY_WINDOW_MINUTES at the ctor
// minimum (span range start -> entry cutoff), so every combination is valid
// by construction — a deeper window adds nothing because bars are selected
// by date. Tune the values here; makeNyOpenRangeBreakoutStrategy reads back
// every name registered below.
ParameterGenerator buildNyOpenRangeBreakoutStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kNyOpenRangeBreakoutSymbolGroupsOverride>();
    // Signal timeframe the pre-open range is built from. 30 dropped in the
    // probe grid.
    generator.addList("OHLC_MINUTES", {5, 15});
    // Range depth in hours back from the open: 4 = tight pre-open coil,
    // 13 = the whole overnight session (the ctor caps at 13 so the range
    // never reaches past the previous UTC midnight).
    generator.addList("RANGE_HOURS", {4, 8, 13});
    // Padding on the pre-open high/low, in pips (0 = raw range).
    generator.addList("BUFFER_PIPS", {0, 2});
    // How long after the NY open entries may fire. The long windows carried
    // no winners in 2026-28 — the probe keeps the first hour.
    generator.addList("ENTRY_WINDOW_MINUTES", {30, 60});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock (an uncapped live winner rode a trade for hours).
    // The trend family's winners pinned at the old 120 edge, so the probe
    // keeps 120 and tries 240.
    generator.addList("MAX_TRADE_DURATION_MINUTES", {120, 240});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped). The wide-stop rationale
    // failed its A/B (50 of the 56 winners sat at stop 1) — the probe pins
    // stop 1, single-value so the mapper's getInt contract holds.
    generator.addList("STOP_DISTANCE_IN_ATR", {1});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {3, 5});
    return generator;
}

}  // namespace sweep
