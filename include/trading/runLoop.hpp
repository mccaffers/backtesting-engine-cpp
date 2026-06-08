// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <vector>
#include "tradeManager.hpp"
#include "reviewStopAndLimit.hpp"
#include "models/priceData.hpp"
#include "strategies/strategy.hpp"                 // IStrategy
#include "trading_definitions/trading_variables.hpp"

namespace trading {

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
inline void runTicks(TradeManager& tradeManager,
                     IStrategy& strategy,
                     const std::vector<PriceData>& ticks,
                     const trading_definitions::TradingVariables& vars) {
    for (const auto& tick : ticks) {

        // Close any trade whose stop-loss or take-profit fired on this tick
        // before we consider opening a new one otherwise an exit and an
        // entry could race within the same tick.
        trading::reviewStopAndLimit(tradeManager, tick);

        if (!tradeManager.hasActiveTradeForSymbol(tick.symbol)) {
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
}

}  // namespace trading
