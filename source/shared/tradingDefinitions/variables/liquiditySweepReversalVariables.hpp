// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the liquidity sweep reversal strategy. PIVOT_BARS is the
// fractal wing: a closed bar is a swing high/low when its extreme STRICTLY
// beats the PIVOT_BARS closed bars on each side (ties disqualify), so a pivot
// only exists once its right wing has fully closed — no lookahead.
// LOOKBACK_BARS bounds how far back pivots are scanned. MIN_SWEEP_PIPS is the
// minimum wick excursion beyond a swept level (converted to integer points
// via symbol_scale::get) for the touch to count as a sweep rather than a
// shallow tap; 0 accepts any strict poke. DISPLACEMENT_ATR_TENTHS demands the
// rejection bar's body be at least tenths x ATR(10) / 10 against the sweep —
// 0 disables the gate, so the sweep itself A/Bs whether displacement carries
// signal. VALID_BARS is the rejection's freshness window (the squeeze
// breakout idiom): a setup older than this many closed bars leaves the scan.
// MAX_TRADE_DURATION_MINUTES is the during() time cap (strategy_exits::
// closeIfPastCap): a trade open strictly longer closes at the exit-side
// price; <= 0 disables — and the 0 default is what winner configs persisted
// before the field existed parse to (uncapped, their original behaviour).
// WITH_DEFAULT so winner configs persisted before a field existed parse with
// the in-class defaults instead of throwing; the strategy ctor rejects zero
// PIVOT_BARS / LOOKBACK_BARS / VALID_BARS loudly, so an absent required field
// still fails fast — at construction, not at parse.
struct LiquiditySweepReversalVariables {
    int PIVOT_BARS = 0;
    int LOOKBACK_BARS = 0;
    int MIN_SWEEP_PIPS = 0;
    int DISPLACEMENT_ATR_TENTHS = 0;
    int VALID_BARS = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LiquiditySweepReversalVariables,
                                                PIVOT_BARS,
                                                LOOKBACK_BARS,
                                                MIN_SWEEP_PIPS,
                                                DISPLACEMENT_ATR_TENTHS,
                                                VALID_BARS,
                                                MAX_TRADE_DURATION_MINUTES);
}
