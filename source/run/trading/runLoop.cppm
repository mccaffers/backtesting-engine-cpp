// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/decimal.hpp>

#include "shared/tradingDefinitions/runConfiguration.hpp"  // DEFAULT_STARTING_BALANCE
#include "shared/tradingDefinitions/tradingVariables.hpp"

export module runLoop;

import std;                // replaces <cstdint>, <vector>
import tradeManager;       // TradeManager
import reviewStopAndLimit; // trading::reviewStopAndLimit
import priceData;          // PriceData
import strategy;           // IStrategy

export namespace trading {

// Run-level risk limits, shared by every strategy in a sweep (they come from
// RunConfiguration). <= 0 disables the respective check, so unconstrained
// experiments need no extra flag.
struct RiskLimits {
    boost::decimal::decimal64_t startingBalance{
        tradingDefinitions::DEFAULT_STARTING_BALANCE};
    boost::decimal::decimal64_t maxLossPercent{0};
    int maxOpenTrades{0};
    // Points-per-pip of the run's symbol — converts the pip-denominated loss
    // floor into the integer points the PnL is tracked in. Defaults to 1 (floor
    // stays in raw points) for callers/tests that don't set it. Set by
    // Operations::run from the run's symbol; exact for single-asset-class runs.
    int pointsPerPip{1};
};

// How a run ended: ran out of ticks, or was cut off because realized losses
// reached the account loss limit (the fail-fast path).
enum class RunStatus {
    Completed,
    LossLimitBreached,
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
// reported PnL is the true account PnL at the cutoff.
inline RunStatus runTicks(TradeManager& tradeManager,
                          IStrategy& strategy,
                          const std::vector<PriceData>& ticks,
                          const tradingDefinitions::TradingVariables& vars,
                          const RiskLimits& limits = {}) {
    const boost::decimal::decimal64_t zero{0};
    const bool lossLimitActive = limits.maxLossPercent > zero;
    // Lowest equity the run may reach, in int64 points. balance * loss% gives
    // the floor in pips; scaling by points-per-pip puts it in the same integer
    // points the PnL is tracked in, e.g. 10000 at 5% on EURUSD (10 pts/pip) ->
    // -500 pips -> -5000 points. Computed once; the per-tick check is integer.
    const std::int64_t pnlFloorPoints = -static_cast<std::int64_t>(
        limits.startingBalance * limits.maxLossPercent / 100 * limits.pointsPerPip);
    const auto lossLimitBreached = [&] {
        return lossLimitActive &&
               tradeManager.calculatePnl() + tradeManager.unrealizedPnl() <= pnlFloorPoints;
    };

    for (const auto& tick : ticks) {

        // Revalue this symbol's open trades at the new price so the equity
        // check below sees current floating drawdown, not stale marks.
        tradeManager.markToMarket(tick);

        // Close any trade whose stop-loss or take-profit fired on this tick
        // before we consider opening a new one otherwise an exit and an
        // entry could race within the same tick.
        trading::reviewStopAndLimit(tradeManager, tick);

        if (lossLimitBreached()) {
            tradeManager.closeAllTrades(tick);
            return RunStatus::LossLimitBreached;
        }

        // Entry gates: at most one open trade per symbol, and (when capped)
        // no more than maxOpenTrades positions across the whole run. The cap
        // is checked first so a capped run skips decide() entirely.
        const bool belowOpenTradeCap =
            limits.maxOpenTrades <= 0 ||
            tradeManager.reviewAccount() <
                static_cast<std::size_t>(limits.maxOpenTrades);
        if (belowOpenTradeCap &&
            !tradeManager.hasActiveTradeForSymbol(tick.symbol)) {
            if (auto signal = strategy.decide(tick)) {
                tradeManager.openTrade(tick,
                                       vars.TRADING_SIZE,
                                       *signal,
                                       vars.STOP_DISTANCE_IN_PIPS,
                                       vars.LIMIT_DISTANCE_IN_PIPS);
            }
        }

        // Strategy-driven management hook for non-SL/TP exit logic
        // (e.g. trailing stops, partial closes). The default
        // RandomStrategy implementation is a no-op now that exits are
        // handled by reviewStopAndLimit above.
        strategy.during(tick, tradeManager);
    }

    // Entries and during() run after the per-tick check; catch a breach on
    // the final tick so the outcome is reported consistently.
    if (lossLimitBreached()) {
        if (!ticks.empty()) {
            tradeManager.closeAllTrades(ticks.back());
        }
        return RunStatus::LossLimitBreached;
    }
    return RunStatus::Completed;
}

}  // namespace trading
