// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "operations.hpp"
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include "tradeManager.hpp"
#include "reviewStopAndLimit.hpp"
#include "reporting.hpp"
#include "strategies/strategy.hpp"
#include "strategies/randomStrategy.hpp"
#include "strategies/strategyErrors.hpp"

namespace {

// Adding a new strategy means adding one branch here; nothing else in
// Operations needs to know about the concrete type.
std::unique_ptr<IStrategy>
selectStrategy(const trading_definitions::Configuration& config) {
    const auto& name = config.STRATEGY.TRADING_VARIABLES.STRATEGY;
    if (name == "RandomStrategy") {
        return std::make_unique<RandomStrategy>(config.STRATEGY);
    }
    throw UnknownStrategyError(name);
}

} // namespace

void Operations::run(const std::vector<PriceData>& ticks,
                     const trading_definitions::Configuration& config) {

    auto tradeManager = std::make_unique<TradeManager>();
    auto strategy = selectStrategy(config);

    const auto& tradingVars = config.STRATEGY.TRADING_VARIABLES;

    for (const auto& tick : ticks) {

        // Close any trade whose stop-loss or take-profit fired on this tick
        // before we consider opening a new one — otherwise an exit and an
        // entry could race within the same tick.
        trading::reviewStopAndLimit(*tradeManager, tick);

        if (!tradeManager->hasActiveTradeForSymbol(tick.symbol)) {
            if (auto signal = strategy->decide(tick)) {
                tradeManager->openTrade(tick,
                                        tradingVars.TRADING_SIZE,
                                        *signal,
                                        tradingVars.STOP_DISTANCE_IN_PIPS,
                                        tradingVars.LIMIT_DISTANCE_IN_PIPS);
            }
        }

        // Strategy-driven management hook for non-SL/TP exit logic
        // (e.g. trailing stops, partial closes). The default
        // RandomStrategy implementation is a no-op now that exits are
        // handled by reviewStopAndLimit above.
        strategy->during(tick, *tradeManager);
    }

    Reporting::summarise(*tradeManager);
}
