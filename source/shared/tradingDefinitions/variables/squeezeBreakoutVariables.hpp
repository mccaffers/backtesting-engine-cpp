// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the volatility-squeeze breakout strategy. NR_LOOKBACK picks
// the contraction pattern: 0 = inside bar (the newest closed bar's range sits
// entirely within its predecessor's), N >= 2 = NR-N (the bar's high-low range
// is STRICTLY the narrowest of the last N closed bars). 1 is rejected by the
// strategy ctor — "narrowest of the last one" matches every bar. VALID_BARS
// is how many closed bars back a matched pattern may sit and still supply
// breakout levels (1 = the newest closed bar only); the pattern's own
// high/low are the levels. BUFFER_PIPS pads those levels (converted to
// integer points via symbol_scale::get) — a noise filter against marginal
// pokes. MAX_TRADE_DURATION_MINUTES is the during() time cap
// (strategy_exits::closeIfPastCap): a trade open strictly longer closes at
// the exit-side price; <= 0 disables — and the 0 default is what winner
// configs persisted before the field existed parse to (uncapped, their
// original behaviour). WITH_DEFAULT so winner configs persisted before a
// field existed parse with the in-class defaults instead of throwing;
// NR_LOOKBACK's 0 default is the (valid) inside-bar mode, and the ctor
// rejects the zero VALID_BARS loudly, so a wholly absent group still fails
// fast — at construction, not at parse.
struct SqueezeBreakoutVariables {
    int NR_LOOKBACK = 0;
    int VALID_BARS = 0;
    int BUFFER_PIPS = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SqueezeBreakoutVariables,
                                                NR_LOOKBACK,
                                                VALID_BARS,
                                                BUFFER_PIPS,
                                                MAX_TRADE_DURATION_MINUTES);
}
