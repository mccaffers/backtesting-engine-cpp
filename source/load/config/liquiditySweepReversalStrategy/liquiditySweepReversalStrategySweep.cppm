// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module liquiditySweepReversalStrategySweep;

export import parameterGenerator;  // buildLiquiditySweepReversalStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide. The 2026-28 full-universe run let the results decide (the
// KeltnerFade lesson): the highest average score of any family, but the
// ranging crosses and copper (NZDUSD, USDCHF, EURGBP, EURCHF, EURNOK,
// AUDNZD, COPPERCMDUSD) produced zero winners across EVERY strategy and
// are dropped from the sweep universe.
inline constexpr std::array kLiquiditySweepReversalSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "AUDUSD",       "EURUSD",        "GBRIDXGBP",   "GBPUSD",
    "USDJPY",      "GBPJPY",        "EURJPY",      "USDCAD",    "FRAIDXEUR",
    "USA500IDXUSD", "AUSIDXAUD",    "XAUUSD",
    "XAGUSD",      "USATECHIDXUSD", "DEUIDXEUR",   "USA30IDXUSD",
    "LIGHTCMDUSD", "JPNIDXJPY",     "BRENTCMDUSD", "EURAUD",
    "HKGIDXHKD",   "USDSEK"});

// Declares which parameters to sweep for the LiquiditySweepReversalStrategy.
// The OHLC COUNT is NOT swept: makeLiquiditySweepReversalStrategy derives it
// at the ctor minimum (max(LOOKBACK_BARS + PIVOT_BARS + 1, VALID_BARS + 11)),
// so every combination is valid by construction — a deeper window adds
// nothing because the scan depth is governed by LOOKBACK_BARS. Tune the
// values here; makeLiquiditySweepReversalStrategy reads back every name
// registered below.
ParameterGenerator buildLiquiditySweepReversalStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kLiquiditySweepReversalSymbolGroupsOverride>();
    // Signal timeframe the pivots, sweeps and rejections form on.
    generator.addList("OHLC_MINUTES", {15, 30, 60});
    // Fractal wing: closed bars strictly beaten on each side of a pivot.
    generator.addList("PIVOT_BARS", {2, 3, 5});
    // How far back pivots are scanned, in closed bars. 96 probes whether
    // older levels still attract sweeps (widened from the pinned 48).
    generator.addList("LOOKBACK_BARS", {48, 96});
    // Minimum wick excursion beyond the level for a sweep, in pips
    // (0 = any strict poke). 2 tracked 0 in 2026-28; 5 scored best, so the
    // grid slides up to probe 10.
    generator.addList("MIN_SWEEP_PIPS", {0, 5, 10});
    // Rejection body demanded, in tenths of ATR(10) — 0 disables the gate.
    // The 2026-28 A/B answered the deferred OB/breaker question: 15 (the
    // high dose) produced ZERO winners from ~42k runs, while 5 held the
    // family's best averages — displacement carries signal at a moderate
    // dose and dies at high dose. 3/7 bracket the peak.
    generator.addList("DISPLACEMENT_ATR_TENTHS", {0, 3, 5, 7});
    // How many closed bars a rejection stays tradeable for. Winners pinned
    // at the old top edge (4) with the book's best cell average — 6/8 are
    // the new frontier; 1 was weakest and is dropped.
    generator.addList("VALID_BARS", {2, 4, 6, 8});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock (trades were riding for 10+ hours live).
    generator.addList("MAX_TRADE_DURATION_MINUTES", {30, 60, 120});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped). The original grid followed
    // the KeltnerFade "limit NEARER than stop" reversal doctrine — the
    // 2026-28 winners refuted it: scores rose monotonically toward the
    // limit=3 edge and the best cell was limit == stop, so the limit grid
    // now reaches PAST the stop (4/5 are the new frontier).
    generator.addList("STOP_DISTANCE_IN_ATR", {2, 3});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {2, 3, 4, 5});
    return generator;
}

}  // namespace sweep
