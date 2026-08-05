// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module fvgStrategySweep;

export import parameterGenerator;  // buildFvgStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide. MIN_GAP_PIPS is pips (converted per symbol via symbol_scale), so
// any symbol set is coherent. The ranging crosses and copper (NZDUSD,
// USDCHF, EURGBP, EURCHF, EURNOK, AUDNZD, COPPERCMDUSD) produced zero
// winners across EVERY strategy in batch 2026-28 and are dropped from the
// sweep universe.
inline constexpr std::array kFvgSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "AUDUSD",       "EURUSD",        "GBRIDXGBP",   "GBPUSD",
    "USDJPY",      "GBPJPY",        "EURJPY",      "USDCAD",    "FRAIDXEUR",
    "USA500IDXUSD", "AUSIDXAUD",    "XAUUSD",
    "XAGUSD",      "USATECHIDXUSD", "DEUIDXEUR",   "USA30IDXUSD",
    "LIGHTCMDUSD", "JPNIDXJPY",     "BRENTCMDUSD", "EURAUD",
    "HKGIDXHKD",   "USDSEK"});

// Declares which parameters to sweep for the FvgStrategy. The OHLC COUNTS
// are NOT swept: makeFvgStrategy derives them at the ctor minimums
// (LOOKBACK_BARS + 3 and HTF_SMA_PERIOD + 2), so every combination is valid
// by construction — deeper windows add nothing because the scan depth is
// governed by LOOKBACK_BARS / MIN_GAP_AGE_BARS, not the window. Tune the
// values here; makeFvgStrategy reads back every name registered below.
ParameterGenerator buildFvgStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kFvgSymbolGroupsOverride>();
    // FVG timeframe: the closed 3-bar gap patterns. 5-minute was the weakest
    // cell at every HTF in the 2026-28 winners — dropped.
    generator.addList("FVG_OHLC_MINUTES", {15, 30});
    // HTF trend timeframe: SMA over its closed closes is the filter. Winners
    // pinned at the old 240 top edge, so the grid extends to 480. Bars build
    // cold from the window's first tick, so 480-minute bars x SMA 100 is
    // already ~7 weeks of warm-up against the ladder's 3-month rungs — the
    // reason HTF_SMA_PERIOD stops at 100 (200 would never warm on a rung).
    generator.addList("HTF_OHLC_MINUTES", {120, 240, 480});
    // How far back the pattern scan may reach on the FVG timeframe. The
    // 2026-28 winners were byte-identical across the old {10..30} range —
    // non-binding — so it collapses to the two ends, kept as a pair to spot
    // if the wider gaps below make depth bind again.
    generator.addList("LOOKBACK_BARS", {10, 30});
    // Smallest tradeable gap in PIPS (converted to points per symbol by the
    // strategy). Winners pinned at the old top edge (20) with the best
    // scores in the family; 2 was dead weight. 35/50 are the new frontier.
    generator.addList("MIN_GAP_PIPS", {5, 10, 20, 35, 50});
    // 50 beat 20 across the board in 2026-28; 100 probes slower (see the
    // HTF_OHLC_MINUTES warm-up note for why 200 is excluded).
    generator.addList("HTF_SMA_PERIOD", {50, 100});
    // Maximum pattern age despite the name (0 = newest pattern only).
    generator.addList("MIN_GAP_AGE_BARS", {0, 2, 5});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock (trades were riding for 10+ hours live).
    generator.addList("MAX_TRADE_DURATION_MINUTES", {30, 60, 120});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped).
    generator.addList("STOP_DISTANCE_IN_ATR", {1, 2});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {3, 5, 7});  // 9 was flat vs 7
    return generator;
}

}  // namespace sweep
