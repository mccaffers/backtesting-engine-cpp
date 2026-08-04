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
// collide. BUFFER_PIPS already converts per symbol via symbol_scale, so any
// symbol set is coherent. The ranging crosses and copper (NZDUSD, USDCHF,
// EURGBP, EURCHF, EURNOK, AUDNZD, COPPERCMDUSD) produced zero winners
// across EVERY strategy in batch 2026-28 and are dropped from the sweep
// universe.
inline constexpr std::array kOhlcBreakoutSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "AUDUSD",       "EURUSD",        "GBRIDXGBP",   "GBPUSD",
    "USDJPY",      "GBPJPY",        "EURJPY",      "USDCAD",    "FRAIDXEUR",
    "USA500IDXUSD", "AUSIDXAUD",    "XAUUSD",
    "XAGUSD",      "USATECHIDXUSD", "DEUIDXEUR",   "USA30IDXUSD",
    "LIGHTCMDUSD", "JPNIDXJPY",     "BRENTCMDUSD", "EURAUD",
    "HKGIDXHKD",   "USDSEK"});

// Declares which parameters to sweep for the OhlcBreakoutStrategy. The
// dimensions multiply fast (loadCommand shows the exact count and asks for
// confirmation before queueing); the grid is budgeted so combos x the
// symbol groups stays well under a million queued runs. Tune the values here;
// makeOhlcBreakoutStrategy reads back every name registered below, and the
// strategy ctor requires OHLC_COUNT >= 2 and OHLC_MINUTES >= 1.
ParameterGenerator buildOhlcBreakoutStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kOhlcBreakoutSymbolGroupsOverride>();
    // Breakout timeframe: the closed-candle range price must clear. Log-ish
    // spacing over the old 5..50 linear span — adjacent 5-step values were
    // near-duplicate strategies.
    generator.addList("BREAKOUT_OHLC_MINUTES", {5, 10, 15, 30, 50});
    generator.addList("BREAKOUT_OHLC_COUNT", {5, 10, 20, 35, 50});
    // Trend timeframe: EMA over its closes (period = count / 2) is the filter.
    // Starts at 40 — a 20-minute "trend" sits at/below the breakout
    // timeframes; the counts give EMA periods 10/30/60, the old span.
    generator.addList("TREND_OHLC_MINUTES", {40, 80, 120});
    generator.addList("TREND_OHLC_COUNT", {20, 60, 120});
    // Padding on the breakout levels, in pips (0 = raw range). 3 was flat
    // against its neighbours in 2026-28 — the A/B keeps only the ends.
    generator.addList("BUFFER_PIPS", {0, 6});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock. 120 was the pinned edge in 2026-28 (30 clearly
    // the worst) — the cap slides up a notch to probe 240.
    generator.addList("MAX_TRADE_DURATION_MINUTES", {60, 120, 240});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped).
    generator.addList("STOP_DISTANCE_IN_ATR", {2, 3});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {2, 4, 6});
    return generator;
}

}  // namespace sweep
