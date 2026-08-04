// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module squeezeBreakoutStrategySweep;

export import parameterGenerator;  // buildSqueezeBreakoutStrategySweep() returns sweep::ParameterGenerator

import std;  // replaces <array>, <string_view>

export namespace sweep {

// The symbol groups THIS sweep runs against (see kSymbolGroupsOverride in
// randomStrategySweep for the full semantics: empty = default set, entries
// are validated against symbol_scale::kTable at compile time). Named
// per-strategy because the sweep modules are imported side by side
// (loadCommand, tests) and two exported sweep::kSymbolGroupsOverride would
// collide. Squeeze breakouts historically favour indices and metals — the
// 2026-28 batch agreed — and the ranging crosses and copper (NZDUSD,
// USDCHF, EURGBP, EURCHF, EURNOK, AUDNZD, COPPERCMDUSD) produced zero
// winners across EVERY strategy, so they are dropped from the sweep
// universe.
inline constexpr std::array kSqueezeBreakoutSymbolGroupsOverride =
    std::to_array<std::string_view>({
    "AUDUSD",       "EURUSD",        "GBRIDXGBP",   "GBPUSD",
    "USDJPY",      "GBPJPY",        "EURJPY",      "USDCAD",    "FRAIDXEUR",
    "USA500IDXUSD", "AUSIDXAUD",    "XAUUSD",
    "XAGUSD",      "USATECHIDXUSD", "DEUIDXEUR",   "USA30IDXUSD",
    "LIGHTCMDUSD", "JPNIDXJPY",     "BRENTCMDUSD", "EURAUD",
    "HKGIDXHKD",   "USDSEK"});

// Declares which parameters to sweep for the SqueezeBreakoutStrategy. The
// signal OHLC COUNT is NOT swept: makeSqueezeBreakoutStrategy derives it at
// the ctor minimum (VALID_BARS + max(1, NR_LOOKBACK - 1) + 1), so every
// combination is valid by construction — a deeper window adds nothing
// because the scan depth is governed by VALID_BARS / NR_LOOKBACK. Tune the
// values here; makeSqueezeBreakoutStrategy reads back every name registered
// below.
ParameterGenerator buildSqueezeBreakoutStrategySweep() {
    ParameterGenerator generator;
    generator.setSymbolGroups<kSqueezeBreakoutSymbolGroupsOverride>();
    // Signal timeframe the contraction patterns form on.
    generator.addList("OHLC_MINUTES", {15, 30, 60});
    // Pattern mode: 0 = inside bar, N = narrowest range of the last N.
    // 5 was decisively the worst mode in 2026-28 while 0 and 7 both paid —
    // dropped.
    generator.addList("NR_LOOKBACK", {0, 7});
    // How many closed bars a matched pattern stays tradeable for. 3 beat 1
    // at the old top edge — 5 is the new frontier.
    generator.addList("VALID_BARS", {1, 3, 5});
    // Padding on the pattern bar's high/low, in pips (0 = raw levels).
    generator.addList("BUFFER_PIPS", {0, 2});
    // Time cap on open trades, in minutes: during() closes a trade open
    // strictly longer than this. No uncapped variant — every winner must
    // carry an exit clock (trades were riding for 10+ hours live).
    generator.addList("MAX_TRADE_DURATION_MINUTES", {30, 60, 120});
    // Trend timeframe: EMA over its closes (period = count / 2) is the
    // filter — the OhlcBreakoutStrategy idiom. Counts give EMA periods
    // 20/40: fast vs slow filter on the same timeframes.
    generator.addList("TREND_OHLC_MINUTES", {60, 120});
    generator.addList("TREND_OHLC_COUNT", {40, 80});
    // Exits are central (Operations enforces SL/TP). The values are ATR
    // multipliers: conditions::check turns them into pip distances per entry
    // (distance = ATR(10) x multiplier, clamped).
    generator.addList("STOP_DISTANCE_IN_ATR", {1, 2});
    generator.addList("LIMIT_DISTANCE_IN_ATR", {3, 5, 7});  // 9 was flat vs 7
    return generator;
}


}  // namespace sweep
