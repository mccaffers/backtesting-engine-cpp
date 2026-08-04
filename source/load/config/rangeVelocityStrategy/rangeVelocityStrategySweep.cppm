// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module rangeVelocityStrategySweep;

export import parameterGenerator;  // buildRangeVelocityStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics; entries are validated against
// symbol_scale::kTable at compile time). Named per-strategy because the
// sweep modules are imported side by side. Deliberately narrower than the
// full universe: formation-speed needs a DENSE tick stream to measure
// anything (a sparse feed makes every bar look slow). Indices and metals
// only: the 2026-28 batch answered the wider question — the FX-major
// control group returned near-zero (EURUSD literally zero), and both oils
// and copper returned zero winners from ~430k runs — so the sweep keeps
// the set that pays.
inline constexpr std::array kRangeVelocitySymbolGroupsOverride =
    std::to_array<std::string_view>({
    "USA500IDXUSD", "USATECHIDXUSD", "USA30IDXUSD",  "DEUIDXEUR",
    "FRAIDXEUR",    "GBRIDXGBP",     "JPNIDXJPY",    "AUSIDXAUD",
    "HKGIDXHKD",    "XAUUSD",        "XAGUSD"});

// Declares which parameters to sweep for the RangeVelocityStrategy. The
// RANGE_COUNT is NOT swept: makeRangeVelocityStrategy derives it at the ctor
// minimum + margin (max(RUN_BARS + SPEED_LOOKBACK_BARS, EXIT_RUN_BARS) + 2),
// so every combination is valid by construction. SPEED_LOOKBACK_BARS is
// registered single-value (the TREND_OHLC_COUNT precedent) so the mapper's
// no-fallback getInt contract holds and widening it later is a one-line
// edit. Tune the values here; makeRangeVelocityStrategy reads back every
// name registered below.
ParameterGenerator buildRangeVelocityStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kRangeVelocitySymbolGroupsOverride>();
    // The rolling tick window the bar threshold derives from — effectively
    // a bars-per-hour knob on a dense feed.
    generator.addList("RANGE_ATR_TICK_WINDOW", {500, 1000, 2500, 5000, 7000});
    // Bar size as a share of the rolling window range: smaller percent =
    // smaller bars = more of them per move. 10 ran at triple the family's
    // runs-per-winner in 2026-28 — dropped.
    generator.addList("RANGE_ATR_PERCENT", {25, 40, 60});
    // K: consecutive same-direction closed bars demanded. 10 was dead in
    // 2026-28 (62 winners from ~590k runs) — dropped.
    generator.addList("RUN_BARS", {3, 5, 8});
    // M: baseline bars whose median formation time is the speed norm.
    generator.addList("SPEED_LOOKBACK_BARS", {10, 20, 32, 40});
    // Bar passes when duration x 100 <= median x this — 100 is the
    // "at the norm" control. The 2026-28 A/B answered against the filter:
    // 100 beat 80 beat 60 on BOTH winner count and score, so the hard
    // 60-demand is dropped; 80 stays as the last live dose. If 100 keeps
    // winning, the speed gate itself is the next thing to question.
    generator.addList("SPEED_RATIO_PERCENT", {80, 100});
    // E: closed bars against the position that close it from during().
    // Winners pinned at the old top edge (8) — 12 is the new frontier; 2/3
    // scored worst and are dropped.
    generator.addList("EXIT_RUN_BARS", {5, 8, 12});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock. 120 was the pinned edge in 2026-28 (30 the worst
    // cell) — the cap slides up a notch to probe 240.
    generator.addList("MAX_TRADE_DURATION_MINUTES", {60, 120, 240});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped). Momentum shape, the
    // OhlcBreakout doctrine (NOT KeltnerFade's): limit FARTHER than stop —
    // let the flow run, cut the failure quickly. Stop 4 was non-binding
    // (scores identical to 2/3 — duplicate variants) and limit 7 was the
    // pinned best edge, so the limit grid slides up to 9.
    generator.addList("STOP_DISTANCE_IN_ATR", {2, 3});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {3, 5, 7, 9});
    return generator;
}

}  // namespace sweep
