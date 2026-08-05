// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/decimal.hpp>

#include "shared/tradingDefinitions/config/runConfiguration.hpp"  // DEFAULT_STARTING_BALANCE
#include "shared/tradingDefinitions/variables/tradingVariables.hpp"

export module runLoop;

import std;                // replaces <chrono>, <cstdint>, <deque>, <optional>,
                           // <span>, <vector>
import barStore;           // bars::BarStore — shared per-symbol bar histories
import entryConditions;    // conditions::check — pre-decide ATR gate
import marketHours;        // market_hours::tradePermitted
import tradeManager;       // TradeManager
import reviewStopAndLimit; // trading::reviewStopAndLimit
import priceData;          // PriceData
import strategy;           // IStrategy
import resultsSummary;     // ResultsSummary::collect — the performance gate's score

export namespace trading {

// Run-level risk limits, shared by every strategy in a sweep (they come from
// RunConfiguration). <= 0 disables the respective check, so unconstrained
// experiments need no extra flag.
struct RiskLimits {
    boost::decimal::decimal64_t startingBalance{
        tradingDefinitions::DEFAULT_STARTING_BALANCE};
    boost::decimal::decimal64_t maxLossPercent{0};
    int maxOpenTrades{0};
    // Cap on trade entries within any sliding 60-second window of TICK time
    // (backtests replay history, so wall clock would be meaningless).
    // Disabled here by default like the other limits; production runs inherit
    // RunConfiguration's default (60) via Operations::run.
    int maxTradesPerMinute{0};
    // Peak-market-hours entry filter (market_hours::tradePermitted): when
    // set, entries are allowed only inside the symbol's session window.
    // Exits — reviewStopAndLimit, loss-limit liquidation, end-of-data
    // closes, during() — are never gated. Off by default like the other
    // limits.
    bool peakHoursOnly{false};
    // Points-per-pip of the run's symbol — converts the pip-denominated loss
    // floor into the integer points the PnL is tracked in. Defaults to 1 (floor
    // stays in raw points) for callers/tests that don't set it. Set by
    // Operations::run from the run's symbol; exact for single-asset-class runs.
    int pointsPerPip{1};
    // Post-run performance gate, evaluated once at the bottom of runTicks:
    // a run that exhausts its ticks only counts as Completed when its
    // performance score EXCEEDS minPerformanceScore AND its decisive trade
    // count (winners + losers — breakevens carry no information and would
    // pad the sample) EXCEEDS minDecisiveTrades; otherwise it returns
    // Underperformed. Each threshold <= 0 disables that check, matching the
    // other limits, so existing tests and unconstrained runs are unaffected.
    boost::decimal::decimal64_t minPerformanceScore{0};
    int minDecisiveTrades{0};
    // Backtest horizon in months, required whenever minPerformanceScore is
    // active: the score annualises returns over this window, and without it
    // the score computes to 0 and the gate can never pass. Set by
    // Operations::run from the run's LAST_MONTHS.
    int lastMonths{0};
};

// How a run ended: ran out of ticks with the performance gate cleared
// (Completed), ran out of ticks but failed the gate — too weak a score or too
// thin a sample to be worth advancing (Underperformed) — or was cut off
// because losses reached the account loss limit (the fail-fast path).
enum class RunStatus {
    Completed,
    LossLimitBreached,
    Underperformed,
};

// The per-tick backtest loop, factored out of Operations::run so it can be
// driven with a deterministic strategy and an inspectable TradeManager in
// tests. Operations::run remains the production entry point — it constructs the
// TradeManager and (random) strategy and delegates the loop here.
//
// Ordering matters: reviewStopAndLimit runs BEFORE the entry check on each
// tick, so a trade that hits its stop/limit closes first and frees its symbol;
// the strategy may then re-enter on that same tick. `strategy` is taken by
// mutable reference because IStrategy::decide is non-const (the real strategy
// mutates RNG state per call).
//
// The loss limit is checked against account equity — realized PnL plus the
// floating (mark-to-market) PnL of open trades, revalued each tick — right
// after the close phase, so a breaching run stops before opening anything
// new. On breach every open trade is liquidated at its last marked price
// (the close side of the most recent tick seen for its symbol), so the
// reported PnL is the true account PnL at the cutoff. A run that exhausts its
// ticks closes any remaining open trades the same way — at their last marks —
// but as ordinary (non-liquidated) closes, so finalPnl/avgPnl account for
// every trade the run opened. A run that survives its ticks is then held to
// the RiskLimits performance gate before it may report Completed.
// `barStore`/`gateSeries` are the shared bar pipeline and the ATR entry
// conditions (see entryConditions). Production (Operations::run) passes the
// run's store — strategy timeframes and gate series registered — plus the
// gate, so entries get dynamic ATR-derived pip distances. Both default off:
// tests that script exact SL/TP geometry get a local empty store and the
// trading variables pass through to openTrade as literal pip distances,
// keeping the loop machinery testable without ATR warm-ups.
inline RunStatus runTicks(TradeManager& tradeManager,
                          IStrategy& strategy,
                          const std::span<const PriceData> ticks,
                          const tradingDefinitions::TradingVariables& vars,
                          const RiskLimits& limits = {},
                          bars::BarStore* barStore = nullptr,
                          const std::optional<bars::SeriesSpec>& gateSeries =
                              std::nullopt) {

    constexpr boost::decimal::decimal64_t zero{0};

    bars::BarStore localBarStore;
    bars::BarStore& sharedBars =
        barStore != nullptr ? *barStore : localBarStore;
    const bool lossLimitActive = limits.maxLossPercent > zero;

    // Lowest equity the run may reach, in int64 points. NOTE: this is a PIP
    // BUDGET, not a currency limit. The engine has no pip-value/notional model,
    // so balance * loss% is read DIRECTLY as a pip count — 10000 at 5% means
    // "500 pips", not "$500" (the two only coincide under the unstated
    // assumption that one unit of size earns 1 currency unit per pip). Scaling
    // by points-per-pip puts the budget in the same integer points the PnL is
    // tracked in (EURUSD at 10 pts/pip -> -5000 points), and scaling by trade
    // size keeps it a budget on PRICE MOVEMENT: PnL is points × size, so an
    // unscaled floor would silently shrink to budget/size pips for any size
    // above 1. Computed once; the per-tick check is integer.
    const std::int64_t sizeScale = std::max<std::int64_t>(1, vars.TRADING_SIZE);
    const std::int64_t pnlFloorPoints = -static_cast<std::int64_t>(
        limits.startingBalance * limits.maxLossPercent / 100 * limits.pointsPerPip)
        * sizeScale;
    const auto lossLimitBreached = [&] {
        return lossLimitActive &&
               tradeManager.calculatePnl() + tradeManager.unrealizedPnl() <= pnlFloorPoints;
    };

    // Timestamps of entries still inside the sliding one-minute rate window.
    // Only pushed to while the cap is active, so it holds at most
    // maxTradesPerMinute entries; ticks arrive time-ordered (ORDER BY
    // timestamp), so evicting from the front is sufficient.
    const bool tradeRateCapActive = limits.maxTradesPerMinute > 0;
    std::deque<std::chrono::system_clock::time_point> recentOpens;

    for (const auto& tick : ticks) {

        // Revalue this symbol's open trades at the new price so the equity
        // check below sees current floating drawdown, not stale marks.
        tradeManager.markToMarket(tick);

        // Close any trade whose stop-loss or take-profit fired on this tick
        // before we consider opening a new one otherwise an exit and an
        // entry could race within the same tick.
        trading::reviewStopAndLimit(tradeManager, tick);

        // Feed the shared bar pipeline on EVERY tick, ungated (a gap would
        // corrupt the ATR and the strategies' histories) and BEFORE the
        // entry gates: the ATR conditions and decide() judge this tick
        // against bar state that already includes it. Deliberate trend-
        // filter consequence (Ryan's call): the tick's own price counts as
        // trend evidence — the filter reads "current price vs trailing
        // EMA", not "previous tick vs EMA".
        sharedBars.update(tick);

        if (lossLimitBreached()) {
            tradeManager.closeAllTrades(tick);
            return RunStatus::LossLimitBreached;
        }

        // Entry gates: (when filtered) only inside the symbol's peak session
        // window, at most one open trade per symbol, (when capped) no more
        // than maxOpenTrades positions across the whole run, and (when
        // capped) no more than maxTradesPerMinute entries in the sliding
        // minute ending at this tick. The gates are checked first so a gated
        // run skips decide() entirely — a skipped entry is skipped, never
        // deferred to a later tick. markToMarket/reviewStopAndLimit above and
        // during() below run on every tick regardless.
        const bool belowOpenTradeCap =
            limits.maxOpenTrades <= 0 ||
            tradeManager.reviewAccount() <
                static_cast<std::size_t>(limits.maxOpenTrades);

        bool belowTradeRateCap = true;
        if (tradeRateCapActive) {
            // The window is half-open, (tick - 60s, tick]: an entry exactly
            // 60 seconds old has aged out and frees its slot on this tick.
            while (!recentOpens.empty() &&
                   tick.timestamp - recentOpens.front() >= std::chrono::minutes{1}) {
                recentOpens.pop_front();
            }
            belowTradeRateCap =
                recentOpens.size() <
                static_cast<std::size_t>(limits.maxTradesPerMinute);
        }

        const bool inSession =
            !limits.peakHoursOnly ||
            market_hours::tradePermitted(tick.symbol, tick.timestamp);

        if (inSession && belowOpenTradeCap && belowTradeRateCap &&
            !tradeManager.hasActiveTradeForSymbol(tick.symbol)) {
            // ATR entry conditions (when wired), BEFORE decide(): spread vs
            // ATR and the volatility floors, producing this entry's dynamic
            // pip distances from the ATR multipliers. A failed check skips
            // the entry — skipped, never deferred, same doctrine as the
            // caps. Gate off (tests): the variables pass through literally.
            std::optional<conditions::Distances> distances;
            const bool conditionsMet =
                !gateSeries.has_value() ||
                (distances = conditions::check(sharedBars, *gateSeries, tick,
                                               vars.STOP_DISTANCE_IN_ATR,
                                               vars.LIMIT_DISTANCE_IN_ATR))
                    .has_value();
            if (conditionsMet) {
                if (auto signal = strategy.decide(tick, sharedBars)) {
                    tradeManager.openTrade(
                        tick, vars.TRADING_SIZE, *signal,
                        distances ? distances->stopPips
                                  : vars.STOP_DISTANCE_IN_ATR,
                        distances ? distances->limitPips
                                  : vars.LIMIT_DISTANCE_IN_ATR);
                    if (tradeRateCapActive) {
                        recentOpens.push_back(tick.timestamp);
                    }
                }
            }
        }

        // Strategy-driven management hook for non-SL/TP exit logic
        // (e.g. trailing stops, partial closes). The default
        // RandomStrategy implementation is a no-op now that exits are
        // handled by reviewStopAndLimit above.
        strategy.during(tick, sharedBars, tradeManager);
    }

    // Entries and during() run after the per-tick check; catch a breach on
    // the final tick so the outcome is reported consistently.
    if (lossLimitBreached()) {
        if (!ticks.empty()) {
            tradeManager.closeAllTrades(ticks.back());
        }
        return RunStatus::LossLimitBreached;
    }
    // Ran out of ticks with positions still open: close them at their last
    // marked prices so finalPnl/avgPnl account for every trade the run opened
    // (max drawdown and tradesOpened already see them; leaving them open would
    // report a rosier finalPnl than the run's own equity curve). These are
    // ordinary end-of-data closes, not loss-limit liquidations.
    if (!ticks.empty()) {
        tradeManager.closeAllTrades(ticks.back(), /*liquidated=*/false);
    }

    // Performance gate (when configured — see RiskLimits): surviving the ticks
    // is necessary but not sufficient. Evaluated after the closes above so the
    // score sees every trade the run opened. ResultsSummary::collect is the
    // same computation the reporting path stores, so this gate and the
    // Elasticsearch documents can never disagree about a run's score.
    if (limits.minPerformanceScore > zero || limits.minDecisiveTrades > 0) {
        const auto stats = ResultsSummary::collect(
            tradeManager, limits.startingBalance, limits.lastMonths,
            limits.pointsPerPip);
        const std::size_t decisiveTrades = stats.winners + stats.losers;
        if (limits.minDecisiveTrades > 0 &&
            decisiveTrades <= static_cast<std::size_t>(limits.minDecisiveTrades)) {
            return RunStatus::Underperformed;
        }
        if (limits.minPerformanceScore > zero &&
            stats.performanceScore <= limits.minPerformanceScore) {
            return RunStatus::Underperformed;
        }
    }
    return RunStatus::Completed;
}

}  // namespace trading
