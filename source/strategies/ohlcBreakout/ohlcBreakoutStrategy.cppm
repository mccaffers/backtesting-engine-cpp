// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// OhlcBreakoutStrategy — range breakout with an EMA trend filter.
//
// Two OHLC timeframes are read per symbol from the loop owner's shared
// BarStore (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the breakout timeframe. The highest high / lowest low
//                       of its CLOSED candles (the in-progress bar is excluded)
//                       define the range, padded by BUFFER_PIPS.
//   OHLC_VARIABLES[1] — the trend timeframe. An EMA over its closes (period =
//                       half the candle count) is the macro trend filter.
//
// Entry: bid breaks above the range top in a macro uptrend -> LONG; ask breaks
// below the range bottom in a macro downtrend -> SHORT. SL/TP exits stay
// central (ATR-derived pip distances enforced by Operations); the one
// strategy-driven exit is the optional time cap: when
// MAX_TRADE_DURATION_MINUTES > 0, during() closes the symbol's trade once it
// has been open strictly longer than that, at the exit-side price (bid for
// LONG, ask for SHORT — the exit_rules convention). In live the runner's
// close-diff turns that closeTrade into a broker CloseIntent, so the strategy
// needs no environment awareness.
//
// Two structural notes that differ from the C# framework:
//  - The strategy owns NO bar state. The loop owner (runLoop / a live worker)
//    registers this strategy's timeframes on its BarStore and updates it once
//    per tick BEFORE decide(), so bar state here already includes the tick
//    being judged: the in-progress trend bar's close IS the current tick,
//    which makes the trend filter read "current price vs trailing EMA" (a
//    tick's own move counts as trend evidence — deliberate; the old
//    in-during() building lagged this by one tick). The breakout range is
//    unaffected in spirit: it still uses CLOSED candles only, though a
//    bar-rolling tick promotes the previous in-progress bar into the range
//    one tick sooner. The same histories feed the pre-decide ATR entry
//    conditions, and the store may hold a deeper window than OHLC_COUNT when
//    the gate registered one on the same timeframe — decide() reads only its
//    own tail.
//  - One strategy instance sees every symbol's ticks interleaved by timestamp
//    (SYMBOLS = "EURUSD,AUDUSD"), and the BarStore keys its histories by
//    symbol — the C# "persistent list per instrument" requirement.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module ohlcBreakoutStrategy;

import std;           // replaces <algorithm>, <chrono>, <cstdint>, <optional>,
                      // <span>, <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import ema;           // ema::calculate (integer EMA)
import symbolScale;   // symbol_scale::get — points-per-pip for the buffer

export class OhlcBreakoutStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframes / BUFFER_PIPS) — a misconfigured run
    // should die loudly at construction, not trade silently wrong.
    explicit OhlcBreakoutStrategy(const tradingDefinitions::StrategyConfig& strategyConfig);

    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override;

    // Strategy-driven exit hook. Bars are built centrally (BarStore, updated
    // by the loop owner every tick), so all that remains here is the optional
    // time cap: close this symbol's trade once it has outlived
    // MAX_TRADE_DURATION_MINUTES. SL/TP exits remain central.
    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override;

private:
    // `{}` value-initializes: OHLCVariables is an aggregate of plain ints with
    // no defaults of its own, so without this the fields would hold
    // indeterminate values until the constructor body assigns them (reading
    // one before that is undefined behaviour). The constructor can't use a
    // member-init list here because it must validate the config first.
    tradingDefinitions::OHLCVariables breakoutCfg{};
    tradingDefinitions::OHLCVariables trendCfg{};
    std::int32_t bufferPips{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap

    // Scratch buffers reused every decide() — cleared, never shrunk, so the
    // per-tick path stops allocating once their capacity settles.
    std::vector<std::int32_t> closesScratch;
    std::vector<std::int32_t> emaScratch;
};

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
OhlcBreakoutStrategy::OhlcBreakoutStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.size() < 2) {
        throw std::invalid_argument(
            "OhlcBreakoutStrategy: OHLC_VARIABLES needs two entries "
            "(breakout timeframe, trend timeframe)");
    }

    breakoutCfg = ohlcVars[0];
    trendCfg = ohlcVars[1];

    for (const auto& cfg : {breakoutCfg, trendCfg}) {
        // COUNT >= 2 so the breakout list always has a closed candle besides
        // the in-progress one, and the trend EMA period (count/2) is >= 1.
        if (cfg.OHLC_COUNT < 2 || cfg.OHLC_MINUTES < 1) {
            throw std::invalid_argument(
                "OhlcBreakoutStrategy: OHLC_COUNT must be >= 2 and OHLC_MINUTES >= 1");
        }
    }
    const auto& breakoutVars = strategyConfig.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES;
    if (!breakoutVars) {
        throw std::invalid_argument(
            "OhlcBreakoutStrategy: STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES is required "
            "(BUFFER_PIPS)");
    }
    bufferPips = breakoutVars->BUFFER_PIPS;
    maxTradeDuration =
        std::chrono::minutes{breakoutVars->MAX_TRADE_DURATION_MINUTES};
}

void OhlcBreakoutStrategy::during(const PriceData& price,
                                  const bars::BarStore& /*barStore*/,
                                  TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables. Only
    // the current tick's symbol is checked — each symbol's trade meets its
    // own next tick, which also supplies the right close price. decide() may
    // re-enter on a later tick while the breakout condition still holds (the
    // documented re-fire behaviour).
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

std::optional<Direction> OhlcBreakoutStrategy::decide(const PriceData& tick,
                                                      const bars::BarStore& barStore) {
    // This symbol's bar histories; nullptr means the timeframe was never
    // registered on the store or the symbol has not ticked yet — either way
    // there is nothing to decide on.
    const std::vector<OhlcObject>* breakoutSeries = barStore.find(
        tick.symbol, std::chrono::minutes{breakoutCfg.OHLC_MINUTES});
    const std::vector<OhlcObject>* trendSeries =
        barStore.find(tick.symbol, std::chrono::minutes{trendCfg.OHLC_MINUTES});
    if (breakoutSeries == nullptr || trendSeries == nullptr) {
        return std::nullopt;
    }

    // Warm-up gate: no signals until both timeframes have a full window
    // (C#: `if (ohlcList.Count < totalOHLCCount) return`).
    const auto breakoutCount = static_cast<std::size_t>(breakoutCfg.OHLC_COUNT);
    const auto trendCount = static_cast<std::size_t>(trendCfg.OHLC_COUNT);
    if (breakoutSeries->size() < breakoutCount ||
        trendSeries->size() < trendCount) {
        return std::nullopt;
    }

    // Read only this strategy's tail of each history: the store keeps the
    // LARGEST window registered per timeframe, so another consumer (the ATR
    // entry gate) may have deepened a series beyond OHLC_COUNT.
    const std::span<const OhlcObject> breakoutBars =
        std::span(*breakoutSeries).last(breakoutCount);
    const std::span<const OhlcObject> trendBars =
        std::span(*trendSeries).last(trendCount);

    // --- 1. THE BREAKOUT LOGIC ---
    // Range from the CLOSED breakout candles (drop the in-progress last bar —
    // C#: closedCandles = Take(Count - 1)), padded by the pip buffer. Pips
    // scale UP to points here (pips * pointsPerPip); the C# divided because
    // its prices were raw decimals. An unknown symbol returns scale 0, which
    // just disables the buffer rather than corrupting the levels.
    std::int32_t highestHigh = std::numeric_limits<std::int32_t>::min();
    std::int32_t lowestLow = std::numeric_limits<std::int32_t>::max();
    for (const OhlcObject& bar : breakoutBars.first(breakoutBars.size() - 1)) {
        highestHigh = std::max(highestHigh, bar.high);
        lowestLow = std::min(lowestLow, bar.low);
    }
    const std::int32_t bufferPoints = bufferPips * symbol_scale::get(tick.symbol);

    // --- 2. THE TREND FILTER LOGIC ---
    // EMA over the trend timeframe's closes (chronological, in-progress bar
    // included, like the C#). Period = half the window (C#: count * 0.5m,
    // truncated). Both the close and the EMA are read at the last index —
    // and since the store updated before decide(), that last close is the
    // current tick itself: the comparison is "current price vs trailing
    // EMA" (algebraically, close > EMA-including-it iff close >
    // EMA-excluding-it).
    //
    // KNOWN RISK (accepted): the tick being judged supplies its own trend
    // evidence. An EMA step can never drag the average past the new point,
    // so a single-tick spike above the PRIOR EMA always reads as "uptrend"
    // — there is no way for the spike itself to be on the wrong side of an
    // EMA that includes it. With dense ticking this is indistinguishable
    // from the old one-tick-lagged filter; it diverges exactly at price
    // discontinuities (news, thin liquidity, session opens), where the
    // strategy may buy the very tick of a spike out of a falling market.
    // Two live mitigations: a steep prior fall keeps the trailing EMA far
    // overhead (the spike must clear it, not just the local range), and the
    // pre-decide spread-vs-ATR gate (entryConditions) rejects most news
    // ticks because their spreads blow out before their prices do.
    closesScratch.clear();
    for (const OhlcObject& bar : trendBars) {
        closesScratch.push_back(bar.close);
    }
    const int emaPeriod = static_cast<int>(closesScratch.size() / 2);
    ema::calculate(closesScratch, emaPeriod, emaScratch);
    const std::int32_t currentClose = closesScratch.back();
    const std::int32_t currentEma = emaScratch.back();

    // --- 3. EXECUTION LOGIC ---
    // Re-fires while the condition holds once the position is closed; the run
    // loop's one-trade-per-symbol gate prevents stacking entries.
    if (tick.bid > highestHigh + bufferPoints && currentClose > currentEma) {
        return Direction::LONG;
    }
    if (tick.ask < lowestLow - bufferPoints && currentClose < currentEma) {
        return Direction::SHORT;
    }
    return std::nullopt;
}
