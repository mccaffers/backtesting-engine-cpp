// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// swingPivots — fractal swing high/low detection over OHLC bars of scaled
// INT32 points.
//
// A swing high at index i is a bar whose high STRICTLY exceeds the highs of
// the pivotBars bars on each side (the classic fractal definition; a swing
// low mirrors it on the lows). Ties disqualify — the squeeze breakout's
// "ties lose" doctrine: an equalled extreme is no fresh extreme, and the
// strict inequality also guarantees no bar inside the right wing has reached
// the level, which sweep-detection callers rely on (the first later touch of
// the level must sit BEYOND the confirmation wing).
//
// The predicates are deliberately position-based and allocation-free (the
// SqueezeBreakoutStrategy::isPatternAt shape) so they can run on the per-tick
// hot path: callers scan their own closed-bars window and ask about one index
// at a time. Feed CLOSED bars only — a pivot does not exist until the
// pivotBars bars of its right wing have all closed; passing a span whose last
// element is still in progress would confirm pivots against a moving bar
// (lookahead). Pure integer comparisons throughout.
export module swingPivots;

import std;         // replaces <cstddef>, <span>, <stdexcept>
import ohlcObject;  // OhlcObject bar record

export namespace swing_pivots {

// True when the bar at `index` is a swing high: its high strictly exceeds
// the highs of the pivotBars bars on each side.
//
// Precondition (the caller's window-minimum check proves it, the
// SqueezeBreakoutStrategy convention): index >= pivotBars and
// index + pivotBars < bars.size(). Out-of-range reads are the caller's bug;
// only pivotBars itself is validated — < 1 throws std::invalid_argument
// (atr::calculate's convention), since a wingless "pivot" would match every
// bar.
[[nodiscard]] bool isSwingHighAt(std::span<const OhlcObject> bars,
                                 std::size_t index, int pivotBars) {
    if (pivotBars < 1) {
        throw std::invalid_argument(
            "swing_pivots::isSwingHighAt: pivotBars must be >= 1");
    }
    const std::int32_t high = bars[index].high;
    for (std::size_t wing = 1; wing <= static_cast<std::size_t>(pivotBars);
         ++wing) {
        if (bars[index - wing].high >= high || bars[index + wing].high >= high) {
            return false;
        }
    }
    return true;
}

// Mirror of isSwingHighAt on the lows: strictly below both wings.
[[nodiscard]] bool isSwingLowAt(std::span<const OhlcObject> bars,
                                std::size_t index, int pivotBars) {
    if (pivotBars < 1) {
        throw std::invalid_argument(
            "swing_pivots::isSwingLowAt: pivotBars must be >= 1");
    }
    const std::int32_t low = bars[index].low;
    for (std::size_t wing = 1; wing <= static_cast<std::size_t>(pivotBars);
         ++wing) {
        if (bars[index - wing].low <= low || bars[index + wing].low <= low) {
            return false;
        }
    }
    return true;
}

}  // namespace swing_pivots
