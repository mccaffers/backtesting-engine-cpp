// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// entryConditions — the shared pre-decide entry gate, run identically by the
// backtest loop (runLoop) and the live runner (liveStrategyRunner) so the two
// paths can never diverge on when an entry is allowed or how far its exits
// sit. Port of the C# ATR gate that replaced fixed pip distances.
//
// The trading variables STOP_DISTANCE_IN_ATR / LIMIT_DISTANCE_IN_ATR are
// small integer ATR multipliers, not pips. Per entry attempt, check():
//
//   1. computes ATR(kAtrPeriod) over the gate timeframe's bars (BarStore),
//   2. rejects the entry when the current spread exceeds 30% of the ATR —
//      a spread that wide eats the edge before the trade starts,
//   3. turns the multipliers into pip distances (ATR x multiplier),
//   4. rejects when volatility is too low for the floors (a sub-10-pip stop
//      is inside broker noise), and clamps the rest to sane bounds.
//
// nullopt = skip this entry (never deferred, same doctrine as the caps). The
// floors and clamps live HERE, not at the call sites, so backtest and live
// stay in lockstep by construction.
//
// All integer arithmetic: bar prices and ticks are scaled INT32 points, the
// spread test cross-multiplies instead of dividing, and the floors compare in
// points (pips x points-per-pip) so truncation in the final pip division can
// never disagree with the floor that admitted the entry.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module entryConditions;

import std;          // replaces <algorithm>, <chrono>, <cstdint>, <optional>,
                     // <vector>
import atr;          // atr::calculate — integer ATR in points
import barStore;     // bars::BarStore / bars::SeriesSpec
import ohlcObject;   // OhlcObject bar record
import priceData;    // PriceData
import symbolScale;  // symbol_scale::get — points-per-pip

namespace {

// TODO customise: ATR period and the gate's fallback timeframe are fixed for
// now; revisit once results justify sweeping them (see the sweep configs).
constexpr int kAtrPeriod = 10;

// Fallback gate timeframe for strategies that declare no OHLC bars
// (RandomStrategy): 15-minute bars put ATR(10) on FX majors around 8-15 pips
// — the band where multipliers {1,2} clear the stop floor below.
constexpr std::chrono::minutes kFallbackBarMinutes{15};

// TODO customise: max spread as a fraction of ATR, currently 3/10 = 30%.
// Cross-multiplied in check() so the test stays integer.
constexpr std::int64_t kMaxSpreadNum = 3;
constexpr std::int64_t kMaxSpreadDen = 10;

// Safety bounds on the dynamic distances, in whole pips. Below the floor the
// volatility is too low to trade (the +/-10-pip broker-noise issue); above
// the cap the ATR blew out and the exposure would be silly.
constexpr std::int32_t kMinStopPips = 10;
constexpr std::int32_t kMaxStopPips = 80;
constexpr std::int32_t kMinLimitPips = 3;
constexpr std::int32_t kMaxLimitPips = 300;

}  // namespace

export namespace conditions {

// Dynamic exit distances for one entry attempt, in whole pips — the same
// unit TradeManager::openTrade and OrderIntent already carry, so everything
// downstream of the gate is unchanged.
struct Distances {
    std::int32_t stopPips;
    std::int32_t limitPips;
};

// The bar series the ATR gate reads for a given strategy config: the
// PRIMARY (first) OHLC timeframe — for OhlcBreakoutStrategy that is the
// breakout timeframe — widened to at least kAtrPeriod+1 bars so ATR can
// always warm; or the 15-minute fallback when the strategy builds no bars.
// Register the result on the run's BarStore alongside the strategy's own
// series.
[[nodiscard]] bars::SeriesSpec gateSeriesFor(
    const tradingDefinitions::StrategyConfig& config) {
    if (!config.OHLC_VARIABLES.empty()) {
        const tradingDefinitions::OHLCVariables& primary =
            config.OHLC_VARIABLES.front();
        // {0,0} is the documented "builds no bars" sentinel (RandomStrategy
        // configs carry one — see ohlcVariables.hpp), not a usable timeframe.
        if (primary.OHLC_MINUTES >= 1) {
            return {std::chrono::minutes{primary.OHLC_MINUTES},
                    std::max(primary.OHLC_COUNT, kAtrPeriod + 1)};
        }
    }
    return {kFallbackBarMinutes, kAtrPeriod + 1};
}

// The gate. nullopt = skip this entry: unknown symbol, gate series not warm
// (or dead flat), spread wider than 30% of ATR, or volatility below the pip
// floors. Otherwise the clamped dynamic distances. A multiplier <= 0 yields
// a distance below its floor, so such a config never trades — loud in the
// results rather than silently trading without a stop.
[[nodiscard]] std::optional<Distances> check(const bars::BarStore& store,
                                             const bars::SeriesSpec& gateSeries,
                                             const PriceData& tick,
                                             const std::int32_t stopMultiplier,
                                             const std::int32_t limitMultiplier) {
    const int pointsPerPip = symbol_scale::get(tick.symbol);
    if (pointsPerPip == symbol_scale::kUnknown) {
        return std::nullopt;  // cannot convert points to pips — never trade
    }

    const std::vector<OhlcObject>* candles =
        store.find(tick.symbol, gateSeries.minutes);
    if (candles == nullptr) {
        return std::nullopt;  // series unregistered / symbol not ticked yet
    }
    const std::int64_t atrPoints = atr::calculate(*candles, kAtrPeriod);
    if (atrPoints <= 0) {
        return std::nullopt;  // not warm, or a dead-flat market
    }

    // Spread gate, in points: spread > 30% of ATR <=> spread*10 > ATR*3.
    // Converting both sides to pips first would just divide both by
    // pointsPerPip (and truncate), so the test stays in points.
    const std::int64_t spreadPoints =
        static_cast<std::int64_t>(tick.ask) - tick.bid;
    if (spreadPoints * kMaxSpreadDen > atrPoints * kMaxSpreadNum) {
        return std::nullopt;
    }

    // Dynamic distances in points. Overflow headroom: ATR < 2^24 points and
    // the multipliers are single digits, so the products sit far inside
    // int64 (and inside int32 after the pip division below).
    const std::int64_t stopPointsRaw = atrPoints * stopMultiplier;
    const std::int64_t limitPointsRaw = atrPoints * limitMultiplier;

    // Volatility floors, compared in points (pips x pointsPerPip): passing
    // "raw >= floor * ppp" guarantees the truncated pip division below still
    // yields at least the floor.
    if (stopPointsRaw < std::int64_t{kMinStopPips} * pointsPerPip ||
        limitPointsRaw < std::int64_t{kMinLimitPips} * pointsPerPip) {
        return std::nullopt;
    }

    return Distances{
        .stopPips = std::clamp(
            static_cast<std::int32_t>(stopPointsRaw / pointsPerPip),
            kMinStopPips, kMaxStopPips),
        .limitPips = std::clamp(
            static_cast<std::int32_t>(limitPointsRaw / pointsPerPip),
            kMinLimitPips, kMaxLimitPips),
    };
}

}  // namespace conditions
