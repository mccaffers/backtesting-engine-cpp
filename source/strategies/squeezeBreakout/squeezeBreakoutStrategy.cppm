// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// SqueezeBreakoutStrategy — volatility contraction then expansion: a bar
// whose range has contracted (an inside bar, or the narrowest range of the
// last N) marks a coil, and a break of THAT BAR's high/low trades the
// expansion. Where OhlcBreakoutStrategy needs price to clear a whole
// multi-bar range, this fires off a single compressed bar — the tightest
// setup in the book, and the cheapest: pure OHLC patterns, no indicator on
// the signal timeframe.
//
// Two OHLC timeframes are read per symbol from the loop owner's shared
// BarStore (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the signal timeframe. The last VALID_BARS closed
//                       bars are scanned newest-first for the pattern
//                       NR_LOOKBACK selects: 0 = inside bar (range within
//                       its predecessor's), N >= 2 = NR-N (high-low range
//                       STRICTLY the narrowest of the last N closed bars).
//                       A matched bar's own high/low, padded by BUFFER_PIPS,
//                       are the breakout levels.
//   OHLC_VARIABLES[1] — the trend timeframe. An EMA over its closes (period
//                       = half the candle count) is the macro trend filter,
//                       the exact OhlcBreakoutStrategy idiom — so squeeze
//                       results read as a direct A/B against the range
//                       breakout. The same KNOWN RISK documented there
//                       applies: the judged tick supplies its own trend
//                       evidence.
//
// Entry: bid above a matched pattern bar's padded high in a macro uptrend ->
// LONG; ask below its padded low in a downtrend -> SHORT. Candidates are
// tried newest-first and the first breakout wins; a pattern that matched but
// was not broken does not stop older candidates inside VALID_BARS from
// firing. There is no mitigation concept — once a pattern ages past
// VALID_BARS it simply leaves the scan.
//
// Exits stay fully central (ATR-derived SL/TP enforced by Operations) —
// during() is a no-op, same doctrine as FvgStrategy / KeltnerFadeStrategy.
//
// The same structural notes as the other strategies apply: the store updates
// BEFORE decide() (the in-progress last bar's close IS the current tick), the
// strategy owns no bar state, one instance sees every symbol interleaved, and
// the store may hold a deeper window than OHLC_COUNT when the ATR gate
// registered one on the same timeframe — decide() reads only its own tail.
// The signal re-fires while price holds beyond a valid pattern's level; the
// run loop's one-trade-per-symbol gate prevents stacking.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module squeezeBreakoutStrategy;

import std;           // replaces <algorithm>, <chrono>, <cstddef>, <cstdint>,
                      // <optional>, <span>, <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import ema;           // ema::calculate (integer EMA) — the trend filter
import symbolScale;   // symbol_scale::get — points-per-pip for the buffer

export class SqueezeBreakoutStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframes / variables, windows too small for
    // the scan, the degenerate NR-1) — a misconfigured run should die loudly
    // at construction, not trade silently wrong.
    explicit SqueezeBreakoutStrategy(
        const tradingDefinitions::StrategyConfig& strategyConfig);

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
    tradingDefinitions::OHLCVariables signalCfg{};
    tradingDefinitions::OHLCVariables trendCfg{};
    int nrLookback{};   // 0 = inside-bar mode, >= 2 = NR-N
    int validBars{};
    std::int32_t bufferPips{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap

    // Scratch buffers reused every decide() — cleared, never shrunk, so the
    // per-tick path stops allocating once their capacity settles (the
    // OhlcBreakoutStrategy pattern).
    std::vector<std::int32_t> closesScratch;
    std::vector<std::int32_t> emaScratch;

    // True when the closed bar at `index` (within `closedBars`) is the
    // contraction pattern NR_LOOKBACK selects. The ctor's window minimum
    // guarantees every predecessor the check reads is in range.
    [[nodiscard]] bool isPatternAt(std::span<const OhlcObject> closedBars,
                                   std::size_t index) const;
};

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
SqueezeBreakoutStrategy::SqueezeBreakoutStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.size() < 2) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: OHLC_VARIABLES needs two entries "
            "(signal timeframe, trend timeframe)");
    }

    signalCfg = ohlcVars[0];
    trendCfg = ohlcVars[1];

    for (const auto& cfg : {signalCfg, trendCfg}) {
        if (cfg.OHLC_MINUTES < 1) {
            throw std::invalid_argument(
                "SqueezeBreakoutStrategy: OHLC_MINUTES must be >= 1");
        }
    }
    // COUNT >= 2 keeps the trend EMA period (count / 2) at >= 1.
    if (trendCfg.OHLC_COUNT < 2) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: OHLC_VARIABLES[1].OHLC_COUNT must be >= 2");
    }

    const auto& squeezeVars =
        strategyConfig.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES;
    if (!squeezeVars) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES "
            "is required (NR_LOOKBACK, VALID_BARS)");
    }
    nrLookback = squeezeVars->NR_LOOKBACK;
    validBars = squeezeVars->VALID_BARS;
    bufferPips = squeezeVars->BUFFER_PIPS;

    // NR-1 is degenerate — "strictly the narrowest of the last one" matches
    // every bar, turning the strategy into a permanent breakout scanner.
    if (nrLookback < 0 || nrLookback == 1) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: NR_LOOKBACK must be 0 (inside bar) or >= 2");
    }
    if (validBars < 1) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: VALID_BARS must be >= 1");
    }
    if (bufferPips < 0) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: BUFFER_PIPS must be >= 0");
    }
    if (squeezeVars->MAX_TRADE_DURATION_MINUTES < 0) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: MAX_TRADE_DURATION_MINUTES must be >= 0");
    }
    maxTradeDuration =
        std::chrono::minutes{squeezeVars->MAX_TRADE_DURATION_MINUTES};
    // Window minimum: once decide()'s warm-up gate passes, the signal span is
    // exactly OHLC_COUNT long (closed = OHLC_COUNT - 1). The oldest pattern
    // candidate sits VALID_BARS back and needs its own lookback — one
    // predecessor for the inside bar, NR_LOOKBACK - 1 for NR-N — so this
    // proves every index the scan touches in range.
    const int patternLookback = std::max(1, nrLookback - 1);
    if (signalCfg.OHLC_COUNT < validBars + patternLookback + 1) {
        throw std::invalid_argument(
            "SqueezeBreakoutStrategy: OHLC_VARIABLES[0].OHLC_COUNT must be >= "
            "VALID_BARS + max(1, NR_LOOKBACK - 1) + 1");
    }
}

void SqueezeBreakoutStrategy::during(const PriceData& price,
                                     const bars::BarStore&,
                                     TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables and
    // preserves the original exits-are-central no-op behaviour.
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

bool SqueezeBreakoutStrategy::isPatternAt(
    std::span<const OhlcObject> closedBars, std::size_t index) const {
    if (nrLookback == 0) {
        // Inside bar: the whole range sits within the predecessor's (bounds
        // inclusive — an equal-high/low bar is still "no expansion").
        const OhlcObject& bar = closedBars[index];
        const OhlcObject& mother = closedBars[index - 1];
        return bar.high <= mother.high && bar.low >= mother.low;
    }
    // NR-N: strictly the narrowest high-low range of the last N closed bars
    // (ties lose — a repeat of an earlier width is no new contraction).
    const std::int32_t range =
        closedBars[index].high - closedBars[index].low;
    for (std::size_t back = 1; back < static_cast<std::size_t>(nrLookback);
         ++back) {
        const OhlcObject& other = closedBars[index - back];
        if (range >= other.high - other.low) {
            return false;
        }
    }
    return true;
}

std::optional<Direction> SqueezeBreakoutStrategy::decide(
    const PriceData& tick, const bars::BarStore& barStore) {
    // This symbol's bar histories; nullptr means the timeframe was never
    // registered on the store or the symbol has not ticked yet — either way
    // there is nothing to decide on.
    const std::vector<OhlcObject>* signalSeries = barStore.find(
        tick.symbol, std::chrono::minutes{signalCfg.OHLC_MINUTES});
    const std::vector<OhlcObject>* trendSeries =
        barStore.find(tick.symbol, std::chrono::minutes{trendCfg.OHLC_MINUTES});
    if (signalSeries == nullptr || trendSeries == nullptr) {
        return std::nullopt;
    }

    // Warm-up gate: no signals until both timeframes have a full window.
    const auto signalCount = static_cast<std::size_t>(signalCfg.OHLC_COUNT);
    const auto trendCount = static_cast<std::size_t>(trendCfg.OHLC_COUNT);
    if (signalSeries->size() < signalCount || trendSeries->size() < trendCount) {
        return std::nullopt;
    }

    // Read only this strategy's tail of each history: the store keeps the
    // LARGEST window registered per timeframe, so another consumer (the ATR
    // entry gate) may have deepened a series beyond OHLC_COUNT.
    const std::span<const OhlcObject> signalBars =
        std::span(*signalSeries).last(signalCount);
    const std::span<const OhlcObject> trendBars =
        std::span(*trendSeries).last(trendCount);

    // --- 1. THE TREND FILTER ---
    // The exact OhlcBreakoutStrategy idiom: EMA over the trend timeframe's
    // closes (chronological, in-progress bar included), period = half the
    // window, both close and EMA read at the last index. See that strategy's
    // header for the accepted single-tick-spike risk and its mitigations.
    closesScratch.clear();
    for (const OhlcObject& bar : trendBars) {
        closesScratch.push_back(bar.close);
    }
    const int emaPeriod = static_cast<int>(closesScratch.size() / 2);
    ema::calculate(closesScratch, emaPeriod, emaScratch);
    const std::int32_t currentClose = closesScratch.back();
    const std::int32_t currentEma = emaScratch.back();
    const bool uptrend = currentClose > currentEma;
    const bool downtrend = currentClose < currentEma;
    if (!uptrend && !downtrend) {
        return std::nullopt;
    }

    // --- 2. THE PATTERN SCAN ---
    // Newest-first over the last VALID_BARS closed bars (the last span
    // element is the in-progress bar and never a pattern). Pips scale UP to
    // points (pips x pointsPerPip), the OhlcBreakoutStrategy convention; an
    // unknown symbol returns scale 0, which just disables the buffer rather
    // than corrupting the levels.
    const std::span<const OhlcObject> closedBars =
        signalBars.first(signalBars.size() - 1);
    const std::int32_t bufferPoints = bufferPips * symbol_scale::get(tick.symbol);
    for (std::size_t back = 0; back < static_cast<std::size_t>(validBars);
         ++back) {
        const std::size_t index = closedBars.size() - 1 - back;
        if (!isPatternAt(closedBars, index)) {
            continue;
        }
        const OhlcObject& pattern = closedBars[index];
        if (uptrend && tick.bid > pattern.high + bufferPoints) {
            return Direction::LONG;
        }
        if (downtrend && tick.ask < pattern.low - bufferPoints) {
            return Direction::SHORT;
        }
    }
    return std::nullopt;
}
