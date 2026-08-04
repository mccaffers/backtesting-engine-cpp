// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the range-bar velocity momentum strategy. Range bars all
// cover ~equal price travel (see rangeBarBuilder), so the time each bar took
// to form IS a momentum reading — the clock as a signal, never as a sampling
// basis. RUN_BARS is K: the number of consecutive same-direction closed range
// bars required. SPEED_LOOKBACK_BARS is M: the bars immediately preceding the
// run whose median formation duration is the speed baseline (median, not
// mean, so weekend-gap bars don't poison it). SPEED_RATIO_PERCENT gates each
// run bar: it passes when duration x 100 <= median x this — 100 means "at the
// norm", below 100 demands genuinely faster-than-normal formation.
// EXIT_RUN_BARS is E: a run of this many closed bars AGAINST the open
// position closes it from during() (no speed filter — momentum dying is
// enough). MAX_TRADE_DURATION_MINUTES is the time cap; 0 disables (the
// OhlcBreakout idiom).
// WITH_DEFAULT so winner configs persisted before a field existed parse with
// the in-class defaults instead of throwing; the strategy ctor rejects zeros
// loudly, so an absent required field still fails fast — at construction, not
// at parse.
struct RangeVelocityVariables {
    int RUN_BARS = 0;
    int SPEED_LOOKBACK_BARS = 0;
    int SPEED_RATIO_PERCENT = 0;
    int EXIT_RUN_BARS = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RangeVelocityVariables,
                                                RUN_BARS,
                                                SPEED_LOOKBACK_BARS,
                                                SPEED_RATIO_PERCENT,
                                                EXIT_RUN_BARS,
                                                MAX_TRADE_DURATION_MINUTES);
}
