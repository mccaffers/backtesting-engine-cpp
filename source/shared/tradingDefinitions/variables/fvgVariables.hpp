// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the Fair Value Gap strategy. LOOKBACK_BARS bounds how far
// back the closed-bar scan may reach on the FVG timeframe. MIN_GAP_PIPS is
// the smallest tradeable gap in PIPS — the strategy converts it to points per
// symbol via symbol_scale::get, so one value means the same real-world size
// on every symbol scale (renamed from MIN_GAP_POINTS, which was raw sub-pip
// points; old configs carrying the points field parse to the 0 default and
// the ctor rejects them loudly). HTF_SMA_PERIOD is the SMA length over the
// higher timeframe's closed closes used as the trend filter.
// MIN_GAP_AGE_BARS behaves as a MAXIMUM age despite its name (preserved from
// the C# original): 0 = only the newest closed 3-bar pattern is examined,
// larger values widen the scan backward, clamped by LOOKBACK_BARS.
// MAX_TRADE_DURATION_MINUTES is the during() time cap (strategy_exits::
// closeIfPastCap): a trade open strictly longer closes at the exit-side
// price; <= 0 disables — and the 0 default is what winner configs persisted
// before the field existed parse to (uncapped, their original behaviour).
// WITH_DEFAULT so winner configs persisted before a field existed parse with
// the in-class defaults instead of throwing; the strategy ctor rejects the
// zero defaults loudly, so an absent required field still fails fast — at
// construction, not at parse.
struct FVGStrategyVariables {
    int LOOKBACK_BARS = 0;
    int MIN_GAP_PIPS = 0;
    int HTF_SMA_PERIOD = 0;
    int MIN_GAP_AGE_BARS = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FVGStrategyVariables,
                                                LOOKBACK_BARS,
                                                MIN_GAP_PIPS,
                                                HTF_SMA_PERIOD,
                                                MIN_GAP_AGE_BARS,
                                                MAX_TRADE_DURATION_MINUTES);
}
