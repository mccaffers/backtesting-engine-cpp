// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// RangeVelocityStrategy — velocity momentum on range bars.
//
// Range bars complete when price travels a volatility-scaled threshold (see
// rangeBarBuilder), so every bar covers ~equal distance and the time a bar
// took to form IS a momentum reading: fast bars mean urgent, one-sided flow.
// This is the clock as a SIGNAL — the bars themselves stay purely
// event-driven; formation time is measured over them, never used to build
// them.
//
// Entry: the last RUN_BARS closed range bars all point one way AND each
// formed faster than the recent norm — duration x 100 <= median x
// SPEED_RATIO_PERCENT, where the median is taken over the
// SPEED_LOOKBACK_BARS closed bars immediately preceding the run. Median, not
// mean: a weekend/session-gap bar inflates one baseline element and the
// median shrugs it off, while a gap bar inside the run itself fails the
// speed test — so the strategy naturally refuses to chase the first bars
// after a gap.
//
// Formation durations are open-to-open (OhlcObject carries only the bar's
// first tick time): bar i's duration = bars[i+1].date - bars[i].date,
// overstated by one inter-tick gap, uniformly. The NEWEST closed bar has no
// successor on the tick that closes it, and needs none — the breach is
// happening now, so tick.timestamp - back().date is its EXACT duration.
//
// Fire-once semantics: the whole signal is gated on back().complete, the
// just-closed state that exists exactly on the breach tick (the next tick
// pushes the successor, and a fresh bar can never be born complete —
// threshold >= 1 point). One evaluation per bar close, never on stale
// state; a stop-out mid-run may re-enter on the NEXT fast same-direction
// close — deliberate, and one-trade-per-symbol prevents stacking. In that
// gated state the series holds ONLY closed bars (the in-progress slot IS the
// just-closed back()), which is what makes the ctor's RANGE_COUNT bound
// provable.
//
// Exits: the central ATR stop/limit from the entry gate, plus two
// during() exits — an opposite-direction run of EXIT_RUN_BARS closed bars
// (no speed filter: momentum dying is reason enough to leave) and the
// OhlcBreakout-style max-duration time cap. during() receives the shared
// BarStore precisely for this: decide() is entry-gated and cannot watch an
// open position's bars.
//
// OHLC_VARIABLES is deliberately empty: the ATR entry gate falls back to its
// default 15m series (entryConditions::gateSeriesFor) for stop/limit sizing,
// the same path RandomStrategy takes. A gate-rejected breach tick loses the
// entry (skipped, never deferred) — a wide-spread breach shouldn't enter
// anyway.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module rangeVelocityStrategy;

import std;             // replaces <algorithm>, <chrono>, <cstdint>,
                        // <optional>, <stdexcept>, <vector>
import barStore;        // bars::BarStore — findRange
import ohlcObject;      // OhlcObject bar record (shared with OHLC bars)
import priceData;       // PriceData
import rangeBarBuilder; // rangebar::RangeBarSpec — series identity
import strategy;        // IStrategy
import timeCapExit;     // strategy_exits::closeIfPastCap — the shared time cap
import trade;           // Direction, Trade
import tradeManager;    // TradeManager — during() exits

namespace {

// +1 up-close, -1 down-close, 0 doji. A CLOSED range bar can never be a doji
// (the breaching tick is always a strict new extreme of the bar, so close ==
// high or close == low != open), but the guard is kept defensively — it
// costs nothing and protects against future builder changes.
int barDirection(const OhlcObject& bar) {
    return (bar.close > bar.open) - (bar.close < bar.open);
}

std::int64_t microsOf(const std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               tp.time_since_epoch())
        .count();
}

}  // namespace

export class RangeVelocityStrategy : public IStrategy {
public:
    explicit RangeVelocityStrategy(
        const tradingDefinitions::StrategyConfig& strategyConfig) {
        // Positional contract, like OHLC_VARIABLES: [0] is THE series this
        // strategy trades. operations/liveStrategyCache silently skip
        // zero-sentinel entries when registering, so a malformed entry here
        // would mean a strategy that looks configured but can never see a
        // bar — die loudly at construction instead.
        if (strategyConfig.RANGE_VARIABLES.empty()) {
            throw std::invalid_argument(
                "RangeVelocityStrategy requires RANGE_VARIABLES[0] (the "
                "range-bar series it trades)");
        }
        const auto& rangeVars = strategyConfig.RANGE_VARIABLES[0];
        if (rangeVars.RANGE_ATR_TICK_WINDOW < 1 ||
            rangeVars.RANGE_ATR_PERCENT < 1 || rangeVars.RANGE_COUNT < 1) {
            throw std::invalid_argument(
                "RangeVelocityStrategy: RANGE_VARIABLES[0] fields must all "
                "be >= 1 (a zero-sentinel entry would register no series)");
        }
        spec_ = {.atrTickWindow = rangeVars.RANGE_ATR_TICK_WINDOW,
                 .atrPercent = rangeVars.RANGE_ATR_PERCENT,
                 .count = rangeVars.RANGE_COUNT};

        const auto& vars = strategyConfig.STRATEGY_VARIABLES;
        if (!vars.RANGE_VELOCITY_VARIABLES.has_value()) {
            throw std::invalid_argument(
                "RangeVelocityStrategy requires "
                "STRATEGY_VARIABLES.RANGE_VELOCITY_VARIABLES");
        }
        const auto& rv = *vars.RANGE_VELOCITY_VARIABLES;
        if (rv.RUN_BARS < 1) {
            throw std::invalid_argument(
                "RangeVelocityStrategy: RUN_BARS must be >= 1");
        }
        if (rv.SPEED_LOOKBACK_BARS < 1) {
            throw std::invalid_argument(
                "RangeVelocityStrategy: SPEED_LOOKBACK_BARS must be >= 1");
        }
        if (rv.SPEED_RATIO_PERCENT < 1) {
            throw std::invalid_argument(
                "RangeVelocityStrategy: SPEED_RATIO_PERCENT must be >= 1 "
                "(0 could never admit a bar)");
        }
        if (rv.EXIT_RUN_BARS < 1) {
            throw std::invalid_argument(
                "RangeVelocityStrategy: EXIT_RUN_BARS must be >= 1");
        }
        if (rv.MAX_TRADE_DURATION_MINUTES < 0) {
            throw std::invalid_argument(
                "RangeVelocityStrategy: MAX_TRADE_DURATION_MINUTES must be "
                ">= 0 (0 disables the time cap)");
        }
        runBars = rv.RUN_BARS;
        speedLookbackBars = rv.SPEED_LOOKBACK_BARS;
        speedRatioPercent = rv.SPEED_RATIO_PERCENT;
        exitRunBars = rv.EXIT_RUN_BARS;
        maxTradeDuration = std::chrono::minutes{rv.MAX_TRADE_DURATION_MINUTES};

        // The provable window bound: at breach-tick evaluation every element
        // is closed, so decide() touches indices down to n - K - M (baseline
        // start) with successor lookups capped at n - 1 (the newest bar's
        // duration comes from the tick itself), and during() scans the last
        // E bars. Anything deeper than max(K + M, E) is unused margin.
        const int required = std::max(rv.RUN_BARS + rv.SPEED_LOOKBACK_BARS,
                                      rv.EXIT_RUN_BARS);
        if (rangeVars.RANGE_COUNT < required) {
            throw std::invalid_argument(std::format(
                "RangeVelocityStrategy: RANGE_COUNT ({}) must be >= "
                "max(RUN_BARS + SPEED_LOOKBACK_BARS, EXIT_RUN_BARS) ({})",
                rangeVars.RANGE_COUNT, required));
        }
    }

    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override {
        const std::vector<OhlcObject>* series =
            barStore.findRange(tick.symbol, spec_);
        // Fire-once gate: only the breach tick sees back().complete — the
        // successor's first tick resets it. Everything below may assume the
        // series holds closed bars only.
        if (series == nullptr || series->empty() || !series->back().complete) {
            return std::nullopt;
        }
        const std::size_t n = series->size();
        const auto k = static_cast<std::size_t>(runBars);
        const auto m = static_cast<std::size_t>(speedLookbackBars);
        if (n < k + m) {
            return std::nullopt;  // still warming the run + baseline window
        }
        const std::span<const OhlcObject> bars(*series);

        // 1. The run: last K closed bars, one strict direction throughout.
        const int dir = barDirection(bars[n - 1]);
        if (dir == 0) {
            return std::nullopt;
        }
        for (std::size_t i = n - k; i < n - 1; ++i) {
            if (barDirection(bars[i]) != dir) {
                return std::nullopt;
            }
        }

        // 2. Baseline: median open-to-open duration of the M bars preceding
        //    the run. nth_element (upper median, deterministic) beats a full
        //    sort and only runs on breach ticks.
        durationsScratch.clear();
        for (std::size_t i = n - k - m; i < n - k; ++i) {
            durationsScratch.push_back(microsOf(bars[i + 1].date) -
                                       microsOf(bars[i].date));
        }
        const auto medianIt = durationsScratch.begin() +
                              static_cast<std::ptrdiff_t>(m / 2);
        std::ranges::nth_element(durationsScratch, medianIt);
        const std::int64_t median = *medianIt;
        if (median <= 0) {
            return std::nullopt;  // degenerate stream — same "not warm"
                                  // convention as ATR == 0
        }

        // 3. Speed test on every run bar: duration x 100 <= median x ratio,
        //    all int64 (a weekend gap is ~2.6e11 us; x100 is far from
        //    overflow). Earlier run bars measure open-to-open like the
        //    baseline (apples to apples); the newest bar's breach is THIS
        //    tick, so its duration is exact.
        const std::int64_t allowance = median * speedRatioPercent;
        for (std::size_t i = n - k; i + 1 < n; ++i) {
            const std::int64_t duration =
                microsOf(bars[i + 1].date) - microsOf(bars[i].date);
            if (duration * 100 > allowance) {
                return std::nullopt;
            }
        }
        const std::int64_t newestDuration =
            microsOf(tick.timestamp) - microsOf(bars[n - 1].date);
        if (newestDuration * 100 > allowance) {
            return std::nullopt;
        }

        return dir > 0 ? Direction::LONG : Direction::SHORT;
    }

    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override {
        // (a) Time cap first (cheap, bar-independent): the shared
        // closeIfPastCap — strictly greater, <= 0 disables. A true return
        // means the position is gone; nothing left to manage.
        if (strategy_exits::closeIfPastCap(price, tradeManager,
                                           maxTradeDuration)) {
            return;
        }
        const Trade* trade = tradeManager.findActiveTrade(price.symbol);
        if (trade == nullptr) {
            return;
        }
        // Direction read BEFORE closeTrade — the close erases the map node.
        const std::int32_t exitPrice =
            trade->direction == Direction::LONG ? price.bid : price.ask;

        // (b) Opposite run: EXIT_RUN_BARS closed bars all against the
        // position, judged only on breach ticks (same fire-once gate as
        // decide). No speed filter — momentum dying is reason enough. On the
        // entry's own breach tick this can never trigger: the newest closed
        // bar points WITH the entry, so an all-against run is impossible.
        const std::vector<OhlcObject>* series =
            barStore.findRange(price.symbol, spec_);
        if (series == nullptr || series->empty() || !series->back().complete) {
            return;
        }
        const std::size_t n = series->size();
        const auto e = static_cast<std::size_t>(exitRunBars);
        if (n < e) {
            return;
        }
        const int against = trade->direction == Direction::LONG ? -1 : 1;
        for (std::size_t i = n - e; i < n; ++i) {
            if (barDirection((*series)[i]) != against) {
                return;
            }
        }
        tradeManager.closeTrade(price.symbol, exitPrice, price);
    }

private:
    rangebar::RangeBarSpec spec_{};  // RANGE_VARIABLES[0]: identity + depth
    int runBars = 0;                 // K
    int speedLookbackBars = 0;       // M
    int speedRatioPercent = 0;
    int exitRunBars = 0;             // E
    std::chrono::minutes maxTradeDuration{0};  // <= 0 disables
    // Reused per breach tick so the median never allocates in the hot loop
    // (the OhlcBreakout closesScratch idiom).
    std::vector<std::int64_t> durationsScratch;
};
