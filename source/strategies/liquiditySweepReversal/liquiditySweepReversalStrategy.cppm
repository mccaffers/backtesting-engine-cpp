// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// LiquiditySweepReversalStrategy — fade the failed breakout: old swing
// highs/lows are where resting stops pool, and a wick that runs through such
// a level but CLOSES back on the original side has taken that liquidity and
// found no follow-through. The rejection is the signal; the trade is the
// reversal. This is the book's first REVERSAL archetype — every winning
// family so far is trend-continuation / volatility-expansion, so this is the
// deliberate regime diversifier, and unlike the naive band fade
// (KeltnerFadeStrategy) the entry demands structural confirmation: a swept
// level AND a rejection, optionally a displacement-sized one.
//
// One OHLC timeframe is read per symbol from the loop owner's shared BarStore
// (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the signal timeframe. Swing pivots (fractal extremes,
//                       PIVOT_BARS closed bars strictly beaten on each side —
//                       swing_pivots) are scanned over the last LOOKBACK_BARS
//                       closed bars. For each pivot level, the FIRST later
//                       closed bar to trade beyond it decides the level once
//                       and for all:
//                         - closed beyond it        -> breakout, level dead
//                         - poked < MIN_SWEEP_PIPS  -> shallow tap, level dead
//                         - poked >= MIN_SWEEP_PIPS
//                           and closed back inside  -> the REJECTION bar
//                       A rejection is tradeable while it sits within the
//                       last VALID_BARS closed bars (the squeeze breakout's
//                       freshness idiom) and no later bar has closed beyond
//                       the level (breakout resumed). When
//                       DISPLACEMENT_ATR_TENTHS > 0 the rejection bar must
//                       also be a displacement candle against the sweep: body
//                       >= tenths x ATR(10) / 10, with the ATR frozen over
//                       the bars up to and including the rejection — a setup
//                       that qualified once cannot flicker as newer bars move
//                       a trailing ATR, and the judgment sees no post-event
//                       data. 0 disables the gate (and the body-direction
//                       check), so the sweep itself A/Bs displacement.
//
// Entry: the newest valid rejection wins; an older setup never outranks a
// fresher one, and a single outside bar rejecting BOTH a swept high and a
// swept low is ambiguous — refused. A swept high enters SHORT while the BID
// is back below the level; a swept low enters LONG while the ASK is back
// above it — entries judge the trade's own fill side (the KeltnerFade
// doctrine: the spread must not flatter the setup). No trend filter: the
// hypothesis under test is the failed breakout itself.
//
// First-touch-decides makes level consumption derivable from the bars alone —
// the strategy owns no per-symbol state (per-symbol state lives in the
// BarStore), so a shallow tap or a breakout permanently retires a level
// without anything to remember: re-scanning reaches the same verdict every
// tick. There is never a "second sweep" of the same level.
//
// Exits stay fully central (ATR-derived SL/TP enforced by Operations) —
// during() is a no-op, same doctrine as FvgStrategy / KeltnerFadeStrategy.
// Note the sweep inverts the usual exit shape, exactly like KeltnerFade:
// reversion wants the limit NEARER than the stop (take the snap-back,
// survive the excursion).
//
// All arithmetic is integer: prices are scaled int32 points (< 2^23), bodies
// and ATRs are < 2^24, and every product is computed in int64 (body x 10,
// tenths x ATR — comfortable headroom, the atr module's convention). The
// same structural notes as the other strategies apply: the store updates
// BEFORE decide(), one instance sees every symbol interleaved, the store may
// hold a deeper window than OHLC_COUNT (the ATR entry gate may deepen a
// shared timeframe) so decide() reads only its own tail, and the signal
// re-fires while a valid rejection stands; the run loop's
// one-trade-per-symbol gate prevents stacking.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module liquiditySweepReversalStrategy;

import std;           // replaces <chrono>, <cstddef>, <cstdint>, <optional>,
                      // <span>, <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import atr;           // atr::calculate — the displacement yardstick
import swingPivots;   // swing_pivots — fractal swing high/low detection
import symbolScale;   // symbol_scale::get — points-per-pip for the sweep depth

export class LiquiditySweepReversalStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframe / variables, windows too small for the
    // pivot scan or a warm displacement ATR) — a misconfigured run should die
    // loudly at construction, not trade silently wrong.
    explicit LiquiditySweepReversalStrategy(
        const tradingDefinitions::StrategyConfig& strategyConfig);

    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override;

    // Strategy-driven exit hook: the optional time cap (strategy_exits::
    // closeIfPastCap). SL/TP exits remain central (ATR-derived, enforced by
    // Operations / exit_rules). For a reversal the cap is the thesis clock —
    // a sweep that has not reverted within the window is a failed setup.
    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override;

private:
    // `{}` value-initializes: OHLCVariables is an aggregate of plain ints with
    // no defaults of its own, so without this the fields would hold
    // indeterminate values until the constructor body assigns them (reading
    // one before that is undefined behaviour). The constructor can't use a
    // member-init list here because it must validate the config first.
    tradingDefinitions::OHLCVariables signalCfg{};
    int pivotBars{};
    int lookbackBars{};
    int minSweepPips{};
    int displacementAtrTenths{};
    int validBars{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap

    // A tradeable rejection: the level that was swept and the closed-bar
    // index of the bar that rejected the sweep. Newest rejection wins.
    struct Setup {
        std::size_t rejection{};
        std::int32_t level{};
        bool found = false;
    };

    // True when the rejection bar at `rejection` clears the displacement
    // gate against the sweep direction (bearish body for a swept high,
    // bullish for a swept low). Always true when the gate is off.
    [[nodiscard]] bool displacementOk(std::span<const OhlcObject> closedBars,
                                      std::size_t rejection,
                                      bool sweptHigh) const;
};

namespace {
// The displacement yardstick's period — pinned to the ATR entry gate's so
// "one displacement" and "one stop unit" share a ruler; deliberately not
// swept.
inline constexpr int kAtrPeriod = 10;
}  // namespace

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
LiquiditySweepReversalStrategy::LiquiditySweepReversalStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.empty()) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: OHLC_VARIABLES needs one entry "
            "(the signal timeframe)");
    }

    signalCfg = ohlcVars[0];

    if (signalCfg.OHLC_MINUTES < 1) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: OHLC_MINUTES must be >= 1");
    }

    const auto& sweepVars =
        strategyConfig.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES;
    if (!sweepVars) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: "
            "STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES is required "
            "(PIVOT_BARS, LOOKBACK_BARS, MIN_SWEEP_PIPS, "
            "DISPLACEMENT_ATR_TENTHS, VALID_BARS)");
    }
    pivotBars = sweepVars->PIVOT_BARS;
    lookbackBars = sweepVars->LOOKBACK_BARS;
    minSweepPips = sweepVars->MIN_SWEEP_PIPS;
    displacementAtrTenths = sweepVars->DISPLACEMENT_ATR_TENTHS;
    validBars = sweepVars->VALID_BARS;

    if (pivotBars < 1) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: PIVOT_BARS must be >= 1");
    }
    // The lookback must fit at least one pivot plus its right wing, or the
    // scan range is empty and the config can NEVER trade.
    if (lookbackBars < pivotBars + 1) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: LOOKBACK_BARS must be >= "
            "PIVOT_BARS + 1");
    }
    if (minSweepPips < 0) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: MIN_SWEEP_PIPS must be >= 0");
    }
    if (displacementAtrTenths < 0) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: DISPLACEMENT_ATR_TENTHS must be "
            ">= 0");
    }
    if (validBars < 1 || validBars > lookbackBars) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: VALID_BARS must be >= 1 and <= "
            "LOOKBACK_BARS");
    }
    if (sweepVars->MAX_TRADE_DURATION_MINUTES < 0) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: MAX_TRADE_DURATION_MINUTES must "
            "be >= 0");
    }
    maxTradeDuration =
        std::chrono::minutes{sweepVars->MAX_TRADE_DURATION_MINUTES};
    // Window minimum: once decide()'s warm-up gate passes, the signal span is
    // exactly OHLC_COUNT long (closed = OHLC_COUNT - 1). The closed remainder
    // must cover the full pivot scan (LOOKBACK_BARS of candidates, each
    // needing PIVOT_BARS of left wing beyond the window's oldest candidate)
    // AND keep the displacement ATR warm at the OLDEST valid rejection
    // (kAtrPeriod TRs + the predecessor close, VALID_BARS from the end) —
    // so every index the scan touches is proven in range and a rejection
    // inside the freshness window can never be refused just for a cold ATR.
    const int minCount = std::max(lookbackBars + pivotBars + 1,
                                  validBars + kAtrPeriod + 1);
    if (signalCfg.OHLC_COUNT < minCount) {
        throw std::invalid_argument(
            "LiquiditySweepReversalStrategy: OHLC_VARIABLES[0].OHLC_COUNT must "
            "be >= max(LOOKBACK_BARS + PIVOT_BARS + 1, VALID_BARS + 11)");
    }
}

void LiquiditySweepReversalStrategy::during(const PriceData& price,
                                            const bars::BarStore&,
                                            TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables and
    // preserves the original exits-are-central no-op behaviour.
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

bool LiquiditySweepReversalStrategy::displacementOk(
    std::span<const OhlcObject> closedBars, std::size_t rejection,
    bool sweptHigh) const {
    if (displacementAtrTenths == 0) {
        return true;
    }
    const OhlcObject& bar = closedBars[rejection];
    // The body must point AGAINST the sweep: bearish off a swept high,
    // bullish off a swept low. A doji or wrong-way body is no displacement.
    const std::int64_t body = sweptHigh
                                  ? std::int64_t{bar.open} - bar.close
                                  : std::int64_t{bar.close} - bar.open;
    if (body <= 0) {
        return false;
    }
    // ATR frozen at the rejection bar: only bars up to and including it are
    // read, so the verdict never moves after the event. 0 means not warm /
    // dead-flat — the ctor's window minimum keeps every in-window rejection
    // warm, so 0 here is a genuinely untradeable market (the ATR entry
    // gate's convention).
    const std::int32_t atrPoints =
        atr::calculate(closedBars.first(rejection + 1), kAtrPeriod);
    if (atrPoints <= 0) {
        return false;
    }
    // body >= tenths/10 x ATR without truncating division:
    // body x 10 >= tenths x ATR exactly (both sides int64, operands < 2^24).
    return body * 10 >= std::int64_t{displacementAtrTenths} * atrPoints;
}

std::optional<Direction> LiquiditySweepReversalStrategy::decide(
    const PriceData& tick, const bars::BarStore& barStore) {
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
    // last bar: pivots, sweeps and rejections are all judged on CLOSED bars
    // only — a pivot does not exist until its right wing has closed, and a
    // "rejection" whose close is still moving is lookahead.
    const std::span<const OhlcObject> signalBars =
        std::span(*signalSeries).last(signalCount);
    const std::span<const OhlcObject> closedBars =
        signalBars.first(signalBars.size() - 1);
    const std::size_t n = closedBars.size();

    // Pips scale UP to points (pips x pointsPerPip), the OhlcBreakoutStrategy
    // convention; an unknown symbol returns scale 0, which just relaxes the
    // sweep-depth floor rather than corrupting the levels.
    const std::int32_t sweepPoints =
        minSweepPips * symbol_scale::get(tick.symbol);

    // --- THE PIVOT SCAN ---
    // Candidates need PIVOT_BARS of closed wing on each side and must sit
    // inside the lookback. The ctor's window minimum proves n >=
    // LOOKBACK_BARS + PIVOT_BARS, so the subtraction cannot underflow.
    const auto wing = static_cast<std::size_t>(pivotBars);
    const std::size_t scanStart =
        std::max(wing, n - static_cast<std::size_t>(lookbackBars));
    Setup bestShort;  // newest valid rejection of a swept swing HIGH
    Setup bestLong;   // newest valid rejection of a swept swing LOW

    for (std::size_t i = scanStart; i + wing < n; ++i) {
        // Swept swing high -> SHORT candidate.
        if (swing_pivots::isSwingHighAt(closedBars, i, pivotBars)) {
            const std::int32_t level = closedBars[i].high;
            // The FIRST later bar to trade beyond the level decides it once:
            // the strict pivot guarantees no right-wing bar can be that touch.
            std::size_t j = i + 1;
            while (j < n && closedBars[j].high <= level) {
                ++j;
            }
            if (j < n &&                                   // touched at all
                closedBars[j].close <= level &&            // not a breakout
                closedBars[j].high - level >= sweepPoints  // deep enough
                && j + static_cast<std::size_t>(validBars) >= n  // fresh
                && displacementOk(closedBars, j, /*sweptHigh=*/true)) {
                // Breakout resumed after the rejection kills the setup.
                bool invalidated = false;
                for (std::size_t k = j + 1; k < n; ++k) {
                    if (closedBars[k].close > level) {
                        invalidated = true;
                        break;
                    }
                }
                if (!invalidated &&
                    (!bestShort.found || j > bestShort.rejection)) {
                    bestShort = {.rejection = j, .level = level, .found = true};
                }
            }
        }
        // Swept swing low -> LONG candidate (exact mirror).
        if (swing_pivots::isSwingLowAt(closedBars, i, pivotBars)) {
            const std::int32_t level = closedBars[i].low;
            std::size_t j = i + 1;
            while (j < n && closedBars[j].low >= level) {
                ++j;
            }
            if (j < n &&
                closedBars[j].close >= level &&
                level - closedBars[j].low >= sweepPoints
                && j + static_cast<std::size_t>(validBars) >= n
                && displacementOk(closedBars, j, /*sweptHigh=*/false)) {
                bool invalidated = false;
                for (std::size_t k = j + 1; k < n; ++k) {
                    if (closedBars[k].close < level) {
                        invalidated = true;
                        break;
                    }
                }
                if (!invalidated &&
                    (!bestLong.found || j > bestLong.rejection)) {
                    bestLong = {.rejection = j, .level = level, .found = true};
                }
            }
        }
    }

    // --- THE VERDICT ---
    // Newest rejection wins outright; the loser is discarded, not queued (an
    // older setup never trades while a fresher opposing one stands). The same
    // bar rejecting both ways — an outside bar sweeping a high AND a low — is
    // ambiguous: refused.
    if (bestShort.found && bestLong.found &&
        bestShort.rejection == bestLong.rejection) {
        return std::nullopt;
    }
    const bool shortWins =
        bestShort.found &&
        (!bestLong.found || bestShort.rejection > bestLong.rejection);
    if (shortWins) {
        // Entries judge the trade's own fill side: a SHORT sells the bid,
        // which must be back below the swept level — the spread must not
        // flatter the rejection.
        if (tick.bid < bestShort.level) {
            return Direction::SHORT;
        }
        return std::nullopt;
    }
    if (bestLong.found && tick.ask > bestLong.level) {
        return Direction::LONG;
    }
    return std::nullopt;
}
