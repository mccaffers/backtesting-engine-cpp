// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// ema — integer Exponential Moving Average over scaled INT32 price points.
//
// Port of the C# EMA.Calculate(List<decimal> prices, int period) helper, kept
// entirely in integer arithmetic so it can run on the per-tick hot path (the
// engine's rule: no software-emulated decimal and no floating point there).
//
// Design: the running EMA state lives in an int64 as Q16 fixed point (price
// points x 2^16). The 16 fractional bits are what prevent the classic integer
// EMA "sticking" bug — with whole-point state, small price changes would
// truncate to a zero increment and freeze the indicator; in Q16 the increment
// only vanishes below ~10^-4 of a point, far under the emitted resolution.
//
// The smoothing factor alpha = 2/(period+1) is precomputed once per call as a
// Q16 multiplier (round-to-nearest), so the per-element step is a single
// multiply plus shifts — no division instruction in the loop:
//
//     emaQ += (alphaQ * (priceQ - emaQ) + 2^15) >> 16
//
// This delta form needs one multiply (vs two for the alpha/beta convex form),
// and a constant series is exactly stationary by construction (delta = 0).
// Quantizing alpha to Q16 trades the previous exact rational step for speed:
// |alpha error| <= 2^-17, which perturbs the EMA by at most ~(2^-17/alpha) of
// the price-to-EMA gap — sub-milli-point for any realistic period.
//
// Rounding happens at two independent sites, both round-to-nearest via the
// add-half-then-shift idiom (floor-based, so correct for negative deltas too):
// once folding the Q32 product back into the Q16 state, and once emitting a
// whole-point value. The emit rounding never feeds back into the state, so
// nothing compounds; the state rounding contracts by (1-alpha) each step and
// stays bounded well below one point.
//
// Headroom: the largest scaled price (~4.5M for indices) is < 2^23, so the
// Q16 price is < 2^39 and the Q32 product < 2^55 — comfortably inside int64.
// alphaQ only rounds to zero (a frozen EMA) for periods above ~262k; the
// period here is bounded by the caller's bar-list length, orders of magnitude
// smaller.
export module ema;

import std;  // replaces <algorithm>, <cstdint>, <span>, <stdexcept>, <vector>

namespace {

// Q16 fixed point: 16 fractional bits, i.e. value_in_points * 65536.
constexpr int kFractionBits = 16;
constexpr std::int64_t kHalf = std::int64_t{1} << (kFractionBits - 1);

constexpr std::int64_t toQ(std::int32_t points) {
    return static_cast<std::int64_t>(points) << kFractionBits;
}

// Round-to-nearest back to whole integer points.
constexpr std::int32_t roundToPoints(std::int64_t q) {
    return static_cast<std::int32_t>((q + kHalf) >> kFractionBits);
}

}  // namespace

export namespace ema {

// C# analogue: List<decimal> EMA.Calculate(List<decimal> prices, int period).
//
// `prices` MUST be chronological (index 0 = oldest, last = newest) and holds
// the engine's non-negative scaled points. The output has the same length as
// the input: the first period-1 entries are 0 (not enough data yet), the entry
// at index period-1 is the SMA seed, and every entry after that follows the
// EMA recurrence. Values are rounded to the nearest integer point; the
// recurrence itself always iterates on the full Q16 state, never on the
// rounded outputs.
//
// This overload writes into a caller-owned buffer so per-tick callers can
// reuse capacity instead of allocating — C# analogy: refilling a List you
// keep around instead of newing one up per call.
//
// period < 1 throws std::invalid_argument. Fewer prices than the period yields
// all zeros (defined here; the C# original's behaviour was accidental).
void calculate(std::span<const std::int32_t> prices, int period,
               std::vector<std::int32_t>& out) {
    if (period < 1) {
        throw std::invalid_argument("ema::calculate: period must be >= 1");
    }

    // resize + targeted fills instead of clear/push_back: reused buffers keep
    // their capacity, nothing is zeroed twice, and the loop writes by index.
    out.resize(prices.size());
    const auto n = static_cast<std::size_t>(period);
    if (prices.size() < n) {
        std::ranges::fill(out, 0);
        return;
    }
    std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n) - 1, 0);

    // 1. Seed with the SMA of the first `period` prices, landing at index
    // period-1 exactly like the C#. int64 sum: even absurd periods of maximal
    // prices cannot overflow. Prices are non-negative, so the simple
    // add-half-the-divisor rounding is enough.
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += prices[i];
    }
    std::int64_t emaQ = ((sum << kFractionBits) + period / 2) / period;
    out[n - 1] = roundToPoints(emaQ);

    // 2. Precompute alpha = 2/(period+1) as a Q16 multiplier, round-to-nearest.
    // The only division left after this line is amortised across the call.
    const std::int64_t alphaQ =
        ((std::int64_t{2} << kFractionBits) + (period + 1) / 2) / (period + 1);

    // 3. Hot path: one multiply, two shifts, no division.
    for (std::size_t i = n; i < prices.size(); ++i) {
        const std::int64_t deltaQ = toQ(prices[i]) - emaQ;
        emaQ += (alphaQ * deltaQ + kHalf) >> kFractionBits;
        out[i] = roundToPoints(emaQ);
    }
}

// Convenience overload returning a fresh vector — matches the C# signature
// shape. Prefer the out-param overload on the per-tick path.
[[nodiscard]] std::vector<std::int32_t> calculate(std::span<const std::int32_t> prices,
                                                  int period) {
    std::vector<std::int32_t> out;
    calculate(prices, period, out);
    return out;
}

}  // namespace ema
