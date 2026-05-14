// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "operations.hpp"
// std headers
#include <iostream>
#include <vector>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <iomanip>
#include <cstdio>
#include <ctime>
#include <boost/decimal.hpp>
#include "tradeManager.hpp"
#include "models/symbolScale.hpp"
#include "strategies/strategy.hpp"
#include "strategies/randomStrategy.hpp"

namespace {

// Decide whether the current tick has hit a trade's stop-loss or
// take-profit boundary. Returns the price at which the position would
// close (bid for LONG exits, ask for SHORT exits) along with a flag
// — `std::nullopt` means "no exit on this tick".
//
// Pip → price conversion uses the symbol's scaling factor: a 1.5-pip
// distance on EURUSD (scale 10000) is 0.00015; on USDJPY (scale 100)
// it's 0.015. Trades on unknown symbols (scale 0) are skipped — there
// is no sensible pip distance to apply.
std::optional<boost::decimal::decimal64_t>
checkExit(const Trade& trade, const PriceData& tick) {
    if (trade.scalingFactor == 0) return std::nullopt;
    if (trade.stopDistancePips == 0 &&
        trade.limitDistancePips == 0) {
        return std::nullopt;
    }

    const auto stopOffset  = trade.stopDistancePips  / trade.scalingFactor;
    const auto limitOffset = trade.limitDistancePips / trade.scalingFactor;

    if (trade.direction == Direction::LONG) {
        const auto stopPrice  = trade.entryPrice - stopOffset;
        const auto limitPrice = trade.entryPrice + limitOffset;
        // Exit a long at the bid (the price the broker pays us).
        if (trade.stopDistancePips  != 0 && tick.bid <= stopPrice)  return tick.bid;
        if (trade.limitDistancePips != 0 && tick.bid >= limitPrice) return tick.bid;
    } else {
        const auto stopPrice  = trade.entryPrice + stopOffset;
        const auto limitPrice = trade.entryPrice - limitOffset;
        // Exit a short at the ask (the price we pay to buy back).
        if (trade.stopDistancePips  != 0 && tick.ask >= stopPrice)  return tick.ask;
        if (trade.limitDistancePips != 0 && tick.ask <= limitPrice) return tick.ask;
    }
    return std::nullopt;
}

// Walk every active trade, close any whose SL/TP has been hit on this
// tick. Two-phase to avoid invalidating the map iterator while erasing.
void reviewStopAndLimit(TradeManager& tradeManager, const PriceData& tick) {
    const auto& openTrades = tradeManager.getActiveTrades();
    if (openTrades.empty()) return;

    std::vector<std::pair<std::string, boost::decimal::decimal64_t>> toClose;
    toClose.reserve(openTrades.size());
    for (const auto& [id, trade] : openTrades) {
        if (auto exitPrice = checkExit(trade, tick)) {
            toClose.emplace_back(id, *exitPrice);
        }
    }
    for (const auto& [id, exitPrice] : toClose) {
        tradeManager.closeTrade(id, exitPrice);
    }
}

// Adding a new strategy means adding one branch here; nothing else in
// Operations needs to know about the concrete type.
std::unique_ptr<IStrategy>
selectStrategy(const trading_definitions::Configuration& config) {
    const auto& name = config.STRATEGY.TRADING_VARIABLES.STRATEGY;
    if (name == "RandomStrategy") {
        return std::make_unique<RandomStrategy>(config.STRATEGY);
    }
    throw std::runtime_error("Unknown strategy: '" + name + "'");
}

} // namespace

void Operations::run(const std::vector<PriceData>& ticks,
                     const trading_definitions::Configuration& config) {

    auto tradeManager = std::make_unique<TradeManager>();
    auto strategy = selectStrategy(config);

    const auto& tradingVars = config.STRATEGY.TRADING_VARIABLES;

    std::size_t tickIndex = 0;
    for (const auto& tick : ticks) {

        // Close any trade whose stop-loss or take-profit fired on this tick
        // before we consider opening a new one — otherwise an exit and an
        // entry could race within the same tick.
        reviewStopAndLimit(*tradeManager, tick);

        size_t openTrades = tradeManager->reviewAccount();

        // only open a trade if there is zero
        if (openTrades == 0) {
            // optional is false
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
        strategy->during(tickIndex, tick, *tradeManager);

        ++tickIndex;
    }

    std::cout << "Final PnL: " << std::fixed << std::setprecision(2) << tradeManager->calculatePnl() << std::endl;
}
