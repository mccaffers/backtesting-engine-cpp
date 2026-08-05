// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// Parameters for the Keltner-band mean-reversion strategy. BAND_SMA_PERIOD is
// the lookback over the signal timeframe's CLOSED closes for both the band
// centre (SMA) and its width (ATR over the same closed bars). The band
// half-width is BAND_ATR_MULT_TENTHS x ATR / 10 — tenths so the sweep can
// express fractional multipliers (15 = 1.5x ATR) while the engine stays in
// integer arithmetic. MAX_TRADE_DURATION_MINUTES caps a trade's lifetime
// exactly like the session breakouts': during() closes the symbol's trade
// once it has been open STRICTLY longer than this; <= 0 disables the cap.
// For a fade the cap is the thesis clock — a stretch that has not snapped
// back within the window is a failed reversion, not a position to sit in.
// WITH_DEFAULT so winner configs persisted before a field existed parse with
// the in-class defaults instead of throwing; the strategy ctor rejects the
// zero band defaults loudly, so an absent required field still fails fast —
// at construction, not at parse (an absent cap is simply disabled).
struct KeltnerFadeVariables {
    int BAND_SMA_PERIOD = 0;
    int BAND_ATR_MULT_TENTHS = 0;
    int MAX_TRADE_DURATION_MINUTES = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(KeltnerFadeVariables,
                                                BAND_SMA_PERIOD,
                                                BAND_ATR_MULT_TENTHS,
                                                MAX_TRADE_DURATION_MINUTES);
}
