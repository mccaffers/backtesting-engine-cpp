// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// FvgStrategy — Fair Value Gap retracement entries with a higher-timeframe
// SMA trend filter. Port of the C# FVG strategy; semantics preserved exactly
// (both codebases keep the in-progress bar as the LAST list element, so the
// index conventions map 1:1).
//
// Two OHLC timeframes are read per symbol from the loop owner's shared
// BarStore (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the FVG timeframe. Closed 3-bar patterns (c1,c2,c3)
//                       are scanned newest-first for an unmitigated gap:
//                       bullish when c1.high sits >= the minimum gap below
//                       c3.low (bearish mirrored). A gap is mitigated when
//                       any LATER closed bar traded through it entirely
//                       (bullish: low <= c1.high) — partial fills don't count.
//   OHLC_VARIABLES[1] — the HTF trend timeframe. The SMA of its last
//                       HTF_SMA_PERIOD CLOSED closes vs the last closed close
//                       gates direction: only longs in an uptrend, only
//                       shorts in a downtrend.
//
// Entry: in an HTF uptrend, when the ask retraces INTO a bullish gap
// [c1.high, c3.low] (bounds inclusive) whose last closed bar still closed
// above it -> LONG; mirror with the bid for SHORT. Exits stay fully central
// (ATR-derived SL/TP enforced by Operations) — during() is a no-op.
//
// MIN_GAP_PIPS is in PIPS, converted to points per symbol at decide() time
// via symbol_scale::get (the BUFFER_PIPS pattern from OhlcBreakoutStrategy),
// so one value means the same real-world size on every symbol scale — the
// property that lets the sweep run beyond EURUSD.
//
// MIN_GAP_AGE_BARS behaves as a MAXIMUM age despite its name (preserved from
// the C#): 0 examines only the newest closed pattern, larger values widen the
// scan backward, clamped by LOOKBACK_BARS. Two more quirks carried over
// deliberately: LOOKBACK_BARS = 1 is legal but dead (the scan window is
// empty), and the last-closed-outside-the-gap check is nearly vacuous for the
// newest pattern (the last closed bar IS c3, so it only fails when c3 closes
// exactly on the gap edge) — it bites for aged gaps, where it stops re-entry
// into stagnant ones.
//
// The same structural notes as OhlcBreakoutStrategy apply: the store updates
// BEFORE decide() (the in-progress last bar's close IS the current tick, and
// a bar-rolling tick promotes the previous in-progress bar into the closed
// window one tick sooner than the C#), the store may hold a deeper window
// than OHLC_COUNT when the ATR gate registered one on the same timeframe
// (decide() reads only its own tail), and one instance sees every symbol
// interleaved (per-symbol state lives in the BarStore). The signal re-fires
// every tick while price sits in the gap; the run loop's one-trade-per-symbol
// gate prevents stacking.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module fvgStrategy;

import std;           // replaces <algorithm>, <chrono>, <cstddef>, <cstdint>,
                      // <optional>, <span>, <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import symbolScale;   // symbol_scale::get — points-per-pip for the gap floor

export class FvgStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframes / variables, windows too small for
    // the scan) — a misconfigured run should die loudly at construction, not
    // trade silently wrong.
    explicit FvgStrategy(const tradingDefinitions::StrategyConfig& strategyConfig);

    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override;

    // Strategy-driven exit hook: the optional time cap (strategy_exits::
    // closeIfPastCap). SL/TP exits remain central (ATR-derived, enforced by
    // Operations / exit_rules).
    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override;

private:
    // `{}` value-initializes: OHLCVariables is an aggregate of plain ints with
    // no defaults of its own, so without this the fields would hold
    // indeterminate values until the constructor body assigns them (reading
    // one before that is undefined behaviour). The constructor can't use a
    // member-init list here because it must validate the config first.
    tradingDefinitions::OHLCVariables fvgCfg{};
    tradingDefinitions::OHLCVariables htfCfg{};
    int lookbackBars{};
    std::int32_t minGapPips{};  // pips; decide() converts per symbol
    int htfSmaPeriod{};
    int minGapAgeBars{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap
};

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
FvgStrategy::FvgStrategy(const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.size() < 2) {
        throw std::invalid_argument(
            "FvgStrategy: OHLC_VARIABLES needs two entries "
            "(FVG timeframe, HTF trend timeframe)");
    }

    fvgCfg = ohlcVars[0];
    htfCfg = ohlcVars[1];

    for (const auto& cfg : {fvgCfg, htfCfg}) {
        if (cfg.OHLC_MINUTES < 1) {
            throw std::invalid_argument("FvgStrategy: OHLC_MINUTES must be >= 1");
        }
    }

    const auto& fvgVars = strategyConfig.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES;
    if (!fvgVars) {
        throw std::invalid_argument(
            "FvgStrategy: STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES is required "
            "(LOOKBACK_BARS, MIN_GAP_PIPS, HTF_SMA_PERIOD)");
    }
    lookbackBars = fvgVars->LOOKBACK_BARS;
    minGapPips = fvgVars->MIN_GAP_PIPS;
    htfSmaPeriod = fvgVars->HTF_SMA_PERIOD;
    minGapAgeBars = fvgVars->MIN_GAP_AGE_BARS;

    if (lookbackBars < 1 || minGapPips < 1 || htfSmaPeriod < 1) {
        throw std::invalid_argument(
            "FvgStrategy: LOOKBACK_BARS, MIN_GAP_PIPS and HTF_SMA_PERIOD "
            "must be >= 1");
    }
    if (minGapAgeBars < 0) {
        throw std::invalid_argument("FvgStrategy: MIN_GAP_AGE_BARS must be >= 0");
    }
    if (fvgVars->MAX_TRADE_DURATION_MINUTES < 0) {
        throw std::invalid_argument(
            "FvgStrategy: MAX_TRADE_DURATION_MINUTES must be >= 0");
    }
    maxTradeDuration =
        std::chrono::minutes{fvgVars->MAX_TRADE_DURATION_MINUTES};
    // Window minimums: once decide()'s warm-up gate passes, both spans are
    // exactly OHLC_COUNT long, so these make the C# per-call size checks
    // ("Count < lookbackBars + 3", "Count < htfSmaPeriod + 2") structurally
    // impossible and prove every index in the scan in range.
    if (fvgCfg.OHLC_COUNT < lookbackBars + 3) {
        throw std::invalid_argument(
            "FvgStrategy: OHLC_VARIABLES[0].OHLC_COUNT must be >= LOOKBACK_BARS + 3");
    }
    if (htfCfg.OHLC_COUNT < htfSmaPeriod + 2) {
        throw std::invalid_argument(
            "FvgStrategy: OHLC_VARIABLES[1].OHLC_COUNT must be >= HTF_SMA_PERIOD + 2");
    }
}

void FvgStrategy::during(const PriceData& price, const bars::BarStore&,
                         TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables and
    // preserves the original exits-are-central no-op behaviour.
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

std::optional<Direction> FvgStrategy::decide(const PriceData& tick,
                                             const bars::BarStore& barStore) {
    // This symbol's bar histories; nullptr means the timeframe was never
    // registered on the store or the symbol has not ticked yet — either way
    // there is nothing to decide on.
    const std::vector<OhlcObject>* fvgSeries =
        barStore.find(tick.symbol, std::chrono::minutes{fvgCfg.OHLC_MINUTES});
    const std::vector<OhlcObject>* htfSeries =
        barStore.find(tick.symbol, std::chrono::minutes{htfCfg.OHLC_MINUTES});
    if (fvgSeries == nullptr || htfSeries == nullptr) {
        return std::nullopt;
    }

    // Warm-up gate: no signals until both timeframes have a full window. The
    // ctor guarantees the windows cover the scan (OHLC_COUNT >=
    // LOOKBACK_BARS + 3 / HTF_SMA_PERIOD + 2), so the C# per-call size checks
    // are dropped as redundant.
    const auto fvgCount = static_cast<std::size_t>(fvgCfg.OHLC_COUNT);
    const auto htfCount = static_cast<std::size_t>(htfCfg.OHLC_COUNT);
    if (fvgSeries->size() < fvgCount || htfSeries->size() < htfCount) {
        return std::nullopt;
    }

    // Read only this strategy's tail of each history: the store keeps the
    // LARGEST window registered per timeframe, so another consumer (the ATR
    // entry gate) may have deepened a series beyond OHLC_COUNT.
    const std::span<const OhlcObject> fvgBars = std::span(*fvgSeries).last(fvgCount);
    const std::span<const OhlcObject> htfBars = std::span(*htfSeries).last(htfCount);

    // Signed indices throughout: totalBars - 2 - minGapAgeBars can go
    // negative before the max() clamp, which size_t would wrap into a
    // scan-skipping garbage bound.
    const auto totalBars = static_cast<std::ptrdiff_t>(fvgBars.size());
    const auto htfTotalBars = static_cast<std::ptrdiff_t>(htfBars.size());

    // --- 1. HTF TREND FILTER ---
    // SMA of the last htfSmaPeriod CLOSED closes (the last element is the
    // in-progress bar, so the newest closed bar sits at htfTotalBars - 2 —
    // the C# [htfTotalBars - 2] convention). Sum in int64: closes are int32
    // points and scaled index/metal closes run into the millions, so an int32
    // sum could overflow within a few hundred bars.
    const std::ptrdiff_t htfStartIdx = htfTotalBars - 2;
    std::int64_t htfCloseSum = 0;
    for (std::ptrdiff_t i = htfStartIdx; i > htfStartIdx - htfSmaPeriod; --i) {
        htfCloseSum += htfBars[static_cast<std::size_t>(i)].close;
    }
    // Trend without floating point OR truncating division: close > sum/period
    // <=> close * period > sum exactly (period > 0), reproducing the C#
    // decimal comparison bit-for-bit; a truncated sum/period would misread
    // closes sitting between the truncated and the true SMA.
    const std::int64_t lastHtfCloseScaled =
        std::int64_t{htfBars[static_cast<std::size_t>(htfStartIdx)].close} *
        htfSmaPeriod;
    const bool isHtfUptrend = lastHtfCloseScaled > htfCloseSum;
    const bool isHtfDowntrend = lastHtfCloseScaled < htfCloseSum;

    // --- 2. FVG SCAN ---
    // Closed 3-bar patterns (c1,c2,c3 at i-2,i-1,i), newest-first. The window
    // is bounded below by LOOKBACK_BARS and tightened to the newest
    // MIN_GAP_AGE_BARS + 1 patterns (0 = newest only). The ctor's window
    // minimum keeps every index >= 0: totalBars >= lookbackBars + 3 >= 4, so
    // endSearchIdx >= 2 and i - 2 >= 0.
    //
    // The gap floor is configured in pips; bar prices are integer points, so
    // convert per symbol here (same idiom as OhlcBreakoutStrategy's buffer).
    // In production an unknown scale never reaches decide() — the ATR entry
    // gate rejects those symbols first.
    const std::int32_t minGapPoints = minGapPips * symbol_scale::get(tick.symbol);
    const std::int32_t lastClosedClose =
        fvgBars[static_cast<std::size_t>(totalBars - 2)].close;
    const std::ptrdiff_t startSearchIdx = totalBars - 2;
    const std::ptrdiff_t endSearchIdx =
        std::max<std::ptrdiff_t>(2, totalBars - lookbackBars);
    const std::ptrdiff_t maxAgeIdx =
        std::max(endSearchIdx, totalBars - 2 - minGapAgeBars);

    for (std::ptrdiff_t i = startSearchIdx; i >= maxAgeIdx; --i) {
        const OhlcObject& c3 = fvgBars[static_cast<std::size_t>(i)];
        // c2 (fvgBars[i - 1]) is the displacement bar between c1 and c3; the
        // rule reads only c1/c3, so it is never touched (same as the C#).
        const OhlcObject& c1 = fvgBars[static_cast<std::size_t>(i - 2)];

        // BULLISH gap: c1's high sits >= minGapPoints below c3's low. Both
        // pattern conditions can't hold for one i (that would need c1.high <
        // c1.low), so branch order is irrelevant.
        if (isHtfUptrend && c1.high < c3.low && c3.low - c1.high >= minGapPoints) {
            // Mitigated when ANY strictly-later CLOSED bar traded down
            // through the whole gap (low <= c1.high) — a partial fill leaves
            // the gap live. Empty range when i is the newest pattern.
            bool mitigated = false;
            for (std::ptrdiff_t j = i + 1; j <= totalBars - 2; ++j) {
                if (fvgBars[static_cast<std::size_t>(j)].low <= c1.high) {
                    mitigated = true;
                    break;
                }
            }
            // Entry: the last closed bar still closed above the gap (stops
            // re-entering stagnant aged gaps) and the ask has retraced INTO
            // it, both bounds inclusive.
            if (!mitigated && lastClosedClose > c3.low &&
                tick.ask <= c3.low && tick.ask >= c1.high) {
                return Direction::LONG;
            }
        }

        // BEARISH mirror: c1's low sits >= minGapPoints above c3's high.
        if (isHtfDowntrend && c1.low > c3.high && c1.low - c3.high >= minGapPoints) {
            bool mitigated = false;
            for (std::ptrdiff_t j = i + 1; j <= totalBars - 2; ++j) {
                if (fvgBars[static_cast<std::size_t>(j)].high >= c1.low) {
                    mitigated = true;
                    break;
                }
            }
            if (!mitigated && lastClosedClose < c3.high &&
                tick.bid >= c3.high && tick.bid <= c1.low) {
                return Direction::SHORT;
            }
        }
    }
    return std::nullopt;
}
