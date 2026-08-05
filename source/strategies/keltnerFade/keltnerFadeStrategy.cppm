// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// KeltnerFadeStrategy — mean reversion: fade stretches beyond a volatility
// band back toward the mean. The first strategy in the book that PROFITS from
// chop rather than needing a trend, so it is deliberately anti-correlated
// with OhlcBreakoutStrategy / FvgStrategy (both trend-continuation).
//
// One OHLC timeframe is read per symbol from the loop owner's shared BarStore
// (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the signal timeframe. The band centre is the SMA of
//                       its last BAND_SMA_PERIOD CLOSED closes; the half-width
//                       is BAND_ATR_MULT_TENTHS x ATR / 10, with the ATR taken
//                       over the same closed bars (Keltner-style bands: SMA
//                       +/- k x ATR — chosen over Bollinger because it needs
//                       no integer sqrt).
//
// Entry: the ask stretched STRICTLY below the lower band -> LONG (fade the
// down-move); the bid strictly above the upper band -> SHORT. No trend
// filter: the hypothesis under test is pure reversion to the mean. Exits stay
// central (ATR-derived SL/TP enforced by Operations); the one strategy-driven
// exit is the optional time cap copied from SessionRangeBreakoutStrategy:
// when MAX_TRADE_DURATION_MINUTES > 0, during() closes the symbol's trade
// once open strictly longer than that, at the exit-side price. For a fade
// the cap is the thesis clock — a stretch that has not snapped back within
// the window is a failed reversion, not a position to sit in. Note the sweep
// inverts the usual exit shape: reversion wants the limit NEARER than the
// stop (take the snap-back, survive the excursion).
//
// The band uses CLOSED bars only. The store updates BEFORE decide(), so the
// in-progress last bar's close IS the tick being judged — including it in the
// SMA would drag the band centre toward the very stretch being faded and
// systematically weaken the signal. Excluding the in-progress bar keeps the
// band fixed between bar rolls, mirroring how FvgStrategy reads only closed
// patterns.
//
// All arithmetic is integer: the band comparison is cross-multiplied
// (price x period x 10 vs closeSum x 10 -/+ mult x ATR x period) so no
// truncating division ever moves the band edge — same doctrine as the FVG
// trend filter. The signal re-fires every tick while price sits outside the
// band; the run loop's one-trade-per-symbol gate prevents stacking.
//
// The same structural notes as the other strategies apply: the strategy owns
// no bar state, one instance sees every symbol interleaved (per-symbol state
// lives in the BarStore), and the store may hold a deeper window than
// OHLC_COUNT when the ATR entry gate registered one on the same timeframe —
// decide() reads only its own tail.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module keltnerFadeStrategy;

import std;           // replaces <chrono>, <cstddef>, <cstdint>, <optional>,
                      // <span>, <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import atr;           // atr::calculate — integer ATR for the band width

export class KeltnerFadeStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframe / variables, window too small for the
    // band) — a misconfigured run should die loudly at construction, not
    // trade silently wrong.
    explicit KeltnerFadeStrategy(const tradingDefinitions::StrategyConfig& strategyConfig);

    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override;

    // Strategy-driven exit hook: the optional time cap (see header). SL/TP
    // exits remain central.
    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override;

private:
    // `{}` value-initializes: OHLCVariables is an aggregate of plain ints with
    // no defaults of its own, so without this the fields would hold
    // indeterminate values until the constructor body assigns them (reading
    // one before that is undefined behaviour). The constructor can't use a
    // member-init list here because it must validate the config first.
    tradingDefinitions::OHLCVariables signalCfg{};
    int bandSmaPeriod{};
    int bandAtrMultTenths{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap
};

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
KeltnerFadeStrategy::KeltnerFadeStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.empty()) {
        throw std::invalid_argument(
            "KeltnerFadeStrategy: OHLC_VARIABLES needs one entry "
            "(the signal timeframe)");
    }

    signalCfg = ohlcVars[0];

    if (signalCfg.OHLC_MINUTES < 1) {
        throw std::invalid_argument("KeltnerFadeStrategy: OHLC_MINUTES must be >= 1");
    }

    const auto& fadeVars = strategyConfig.STRATEGY_VARIABLES.KELTNER_FADE_VARIABLES;
    if (!fadeVars) {
        throw std::invalid_argument(
            "KeltnerFadeStrategy: STRATEGY_VARIABLES.KELTNER_FADE_VARIABLES is "
            "required (BAND_SMA_PERIOD, BAND_ATR_MULT_TENTHS)");
    }
    bandSmaPeriod = fadeVars->BAND_SMA_PERIOD;
    bandAtrMultTenths = fadeVars->BAND_ATR_MULT_TENTHS;
    maxTradeDuration =
        std::chrono::minutes{fadeVars->MAX_TRADE_DURATION_MINUTES};

    if (bandSmaPeriod < 1 || bandAtrMultTenths < 1) {
        throw std::invalid_argument(
            "KeltnerFadeStrategy: BAND_SMA_PERIOD and BAND_ATR_MULT_TENTHS "
            "must be >= 1");
    }
    // Window minimum: once decide()'s warm-up gate passes the span is exactly
    // OHLC_COUNT long, whose last element is the in-progress bar. The closed
    // remainder must cover both the SMA (period bars) and the ATR (period + 1
    // bars — each TR needs its predecessor's close), so OHLC_COUNT >=
    // period + 2 proves every read in range and the ATR warm.
    if (signalCfg.OHLC_COUNT < bandSmaPeriod + 2) {
        throw std::invalid_argument(
            "KeltnerFadeStrategy: OHLC_VARIABLES[0].OHLC_COUNT must be >= "
            "BAND_SMA_PERIOD + 2");
    }
}

void KeltnerFadeStrategy::during(const PriceData& price,
                                 const bars::BarStore& /*barStore*/,
                                 TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables. For a
    // fade the cap is the thesis clock — a stretch that has not snapped back
    // within the window is a failed reversion, not a position to sit in.
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

std::optional<Direction> KeltnerFadeStrategy::decide(const PriceData& tick,
                                                     const bars::BarStore& barStore) {
    // This symbol's bar history; nullptr means the timeframe was never
    // registered on the store or the symbol has not ticked yet — either way
    // there is nothing to decide on.
    const std::vector<OhlcObject>* signalSeries = barStore.find(
        tick.symbol, std::chrono::minutes{signalCfg.OHLC_MINUTES});
    if (signalSeries == nullptr) {
        return std::nullopt;
    }

    // Warm-up gate: no signals until the timeframe has a full window.
    const auto signalCount = static_cast<std::size_t>(signalCfg.OHLC_COUNT);
    if (signalSeries->size() < signalCount) {
        return std::nullopt;
    }

    // Read only this strategy's tail of the history (the store keeps the
    // LARGEST window registered per timeframe), then drop the in-progress
    // last bar: the band is built from CLOSED bars only — see the header.
    const std::span<const OhlcObject> signalBars =
        std::span(*signalSeries).last(signalCount);
    const std::span<const OhlcObject> closedBars =
        signalBars.first(signalBars.size() - 1);

    // Band centre: SMA of the last bandSmaPeriod closed closes, kept as an
    // int64 sum (closes are int32 points; scaled index/metal closes run into
    // the millions, so an int32 sum could overflow within a few hundred bars).
    std::int64_t closeSum = 0;
    for (std::size_t i = closedBars.size() - static_cast<std::size_t>(bandSmaPeriod);
         i < closedBars.size(); ++i) {
        closeSum += closedBars[i].close;
    }

    // Band width: ATR over the same closed bars (only the final period + 1
    // are read). 0 means a dead-flat market — a zero-width band would fade
    // every tick of noise, so treat it as untradeable, matching the ATR entry
    // gate's convention.
    const std::int32_t atrPoints = atr::calculate(closedBars, bandSmaPeriod);
    if (atrPoints <= 0) {
        return std::nullopt;
    }

    // Fade a STRICT stretch beyond the band. Without floating point OR
    // truncating division: price < SMA - mult/10 x ATR
    // <=> price x period x 10 < closeSum x 10 - mult x ATR x period exactly
    // (period > 0). Entries judge the trade's own fill side: a LONG buys the
    // ask, a SHORT sells the bid — the spread must not flatter the stretch.
    const std::int64_t bandOffset =
        std::int64_t{bandAtrMultTenths} * atrPoints * bandSmaPeriod;
    const std::int64_t centreScaled = closeSum * 10;

    if (std::int64_t{tick.ask} * bandSmaPeriod * 10 < centreScaled - bandOffset) {
        return Direction::LONG;
    }
    if (std::int64_t{tick.bid} * bandSmaPeriod * 10 > centreScaled + bandOffset) {
        return Direction::SHORT;
    }
    return std::nullopt;
}
