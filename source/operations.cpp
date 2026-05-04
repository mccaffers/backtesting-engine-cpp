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
#include <string>
#include <iomanip>
#include <cstdio>
#include <ctime>
#include <boost/decimal.hpp>
#include "tradeManager.hpp"

void Operations::run(const std::vector<PriceData>& ticks,
                     const trading_definitions::Configuration& config) {

    // Create
    auto tradeManager = new TradeManager();
        
    for (const auto& tick : ticks) {

        size_t openTrades = tradeManager->reviewAccount();
        
        // this would be strategy invoke point
        if (openTrades == 0) {
            std::string tradeId = tradeManager->openTrade(tick, config.STRATEGY.TRADING_VARIABLES.TRADING_SIZE, Direction::LONG);
            std::cout << "Opened trade: " << tradeId << std::endl;
        }

        // this would be a position manager review point
        // randomly check account status every 100 ticks
        if (openTrades > 0 && (std::rand() % 100) == 0) { // NOSONAR(cpp:S2245) experimentation only, not security-sensitive
            std::cout << "Reviewing account at tick timestamp: " << tick.timestamp.time_since_epoch().count() << std::endl;
            std::cout << "Number of open trades: " << openTrades << std::endl;
            for (const auto& [id, trade] : tradeManager->getActiveTrades()) {
                std::cout << "Trade ID: " << id
                        << " | Entry: " << trade.entryPrice
                        << " | Size: " << trade.size
                        << " | Direction: " << (trade.direction == Direction::LONG ? "LONG" : "SHORT")
                        << std::endl;
            }
        }

        // strategy review point
        // randomly close trades every 200 ticks
        if (openTrades > 0 && (std::rand() % 200) == 0) { // NOSONAR(cpp:S2245) experimentation only, not security-sensitive
            std::vector<std::string> idsToClose;
            for (const auto& [id, trade] : tradeManager->getActiveTrades()) {
                idsToClose.push_back(id);
            }
            for (const auto& id : idsToClose) {
                bool closed = tradeManager->closeTrade(id, tick.bid);
                std::cout << "Closed trade ID: " << id << " - " << (closed ? "success" : "failure") << std::endl;
            }
        }
    }

    std::cout << "Final PnL: " << std::fixed << std::setprecision(2) << tradeManager->calculatePnl() << std::endl;

    // bool closed = tradeManager->closeTrade(tradeId);
    // std::cout << "Trade closed: " << (closed ? "yes" : "no") << std::endl;
}
