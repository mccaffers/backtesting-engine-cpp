// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// atr — integer Average True Range over OHLC bars of scaled INT32 points.
//
// Port of the C# AverageTrueRange.Calculate(List<OhlcObject> candles, int
// period) helper, kept entirely in integer arithmetic so it can run on the
// per-tick hot path (the engine's rule: no software-emulated decimal and no
// floating point there — see ema).
//
// True Range of a bar against its predecessor is the largest of:
//   1. high - low                 (the bar's own span)
//   2. |high - prevClose|         (gap up through the previous close)
//   3. |low  - prevClose|         (gap down through the previous close)
// and the ATR here is the plain SMA of the last `period` true ranges — the
// C# original's form, not Wilder's recursive smoothing.
//
// Units: bar prices are the engine's scaled fixed-point points (see
// priceData / symbolScale), so the result is in POINTS. Divide by
// symbol_scale::get(symbol) for pips.
//
// Headroom: the largest scaled price (~4.5M for indices) is < 2^23, so a
// single TR is < 2^24 and the int64 sum cannot overflow for any real period.
export module atr;

import std;         // replaces <cstdint>, <span>, <stdexcept>
import ohlcObject;  // OhlcObject bar record

export namespace atr {

// C# analogue: decimal AverageTrueRange.Calculate(List<OhlcObject>, int).
//
// `candles` MUST be chronological (index 0 = oldest, last = newest); the last
// element may be the in-progress bar — its TR moves until it closes, exactly
// like the C# reference which fed the live list straight in. Only the final
// period+1 candles are read (period TRs, each needing its predecessor's
// close); earlier candles are ignored.
//
// Fewer than period+1 candles returns 0 — the C# behaviour — so callers treat
// 0 as "not warm yet" (a genuinely zero ATR means a dead-flat market, which
// is equally untradeable). period < 1 throws std::invalid_argument.
[[nodiscard]] std::int32_t calculate(std::span<const OhlcObject> candles,
                                     int period = 14) {
    if (period < 1) {
        throw std::invalid_argument("atr::calculate: period must be >= 1");
    }

    const auto n = static_cast<std::size_t>(period);
    if (candles.size() < n + 1) {
        return 0;
    }

    std::int64_t sum = 0;
    for (std::size_t i = candles.size() - n; i < candles.size(); ++i) {
        const std::int64_t prevClose = candles[i - 1].close;
        const std::int64_t highLow =
            static_cast<std::int64_t>(candles[i].high) - candles[i].low;
        const std::int64_t highGap = std::abs(candles[i].high - prevClose);
        const std::int64_t lowGap = std::abs(candles[i].low - prevClose);
        sum += std::max({highLow, highGap, lowGap});
    }

    // TRs are non-negative, so add-half-divisor is exact round-to-nearest
    // (same idiom as ema's SMA seed).
    return static_cast<std::int32_t>((sum + period / 2) / period);
}

}  // namespace atr
