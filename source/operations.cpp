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
#include "exitRules.hpp"
#include "models/symbolScale.hpp"
#include "strategies/strategy.hpp"
#include "strategies/randomStrategy.hpp"

namespace {

// Walk every active trade, close any whose SL/TP has been hit on this
// tick. Two-phase to avoid invalidating the map iterator while erasing.
void reviewStopAndLimit(TradeManager& tradeManager, const PriceData& tick) {
    const auto& openTrades = tradeManager.getActiveTrades();
    if (openTrades.empty()) return;

    std::vector<std::pair<std::string, boost::decimal::decimal64_t>> toClose;
    toClose.reserve(openTrades.size());
    for (const auto& [id, trade] : openTrades) {
        if (auto exitPrice = trading::exit_rules::checkExit(trade, tick)) {
            toClose.emplace_back(id, *exitPrice);
        }
    }
    for (const auto& [id, exitPrice] : toClose) {
        tradeManager.closeTrade(id, exitPrice, tick);
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
        strategy->during(tick, *tradeManager);
    }

    std::cout << "Final PnL: " << std::fixed << std::setprecision(2) << tradeManager->calculatePnl() << std::endl;

    const auto& activeTrades = tradeManager->getActiveTrades();
    const auto& closedTrades = tradeManager->getClosedTrades();

    const std::size_t openedCount = activeTrades.size() + closedTrades.size();
    const std::size_t closedCount = closedTrades.size();

    std::size_t openedLong = 0;
    std::size_t openedShort = 0;
    for (const auto& [id, trade] : activeTrades) {
        if (trade.direction == Direction::LONG) ++openedLong;
        else ++openedShort;
    }

    std::size_t closedLong = 0;
    std::size_t closedShort = 0;
    std::size_t winners = 0;
    std::size_t losers = 0;
    std::size_t breakeven = 0;
    boost::decimal::decimal64_t pnlSum{0};
    const boost::decimal::decimal64_t zero{0};
    for (const auto& trade : closedTrades) {
        if (trade.direction == Direction::LONG) ++closedLong;
        else ++closedShort;
        if (trade.pnl > zero) ++winners;
        else if (trade.pnl < zero) ++losers;
        else ++breakeven;
        pnlSum += trade.pnl;
    }
    openedLong  += closedLong;
    openedShort += closedShort;

    std::cout << "Trades opened: " << openedCount
              << "  (LONG: " << openedLong << ", SHORT: " << openedShort << ")" << std::endl;
    std::cout << "Trades closed: " << closedCount
              << "  (LONG: " << closedLong << ", SHORT: " << closedShort << ")" << std::endl;
    std::cout << "Winners: " << winners
              << "   Losers: " << losers
              << "   Breakeven: " << breakeven << std::endl;
    if (closedCount == 0) {
        std::cout << "Average PnL per closed trade: n/a (0 closed)" << std::endl;
    } else {
        const auto avgPnl = pnlSum / boost::decimal::decimal64_t{static_cast<long long>(closedCount)};
        std::cout << "Average PnL per closed trade: "
                  << std::fixed << std::setprecision(2) << avgPnl << std::endl;
    }
}
