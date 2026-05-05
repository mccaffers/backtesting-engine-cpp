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
#include "strategies/randomStrategy.hpp"

void Operations::run(const std::vector<PriceData>& ticks,
                     const trading_definitions::Configuration& config) {

    // Create
    auto tradeManager = new TradeManager();
    RandomStrategy strategy(config.STRATEGY);

    std::size_t tickIndex = 0;
    for (const auto& tick : ticks) {

        size_t openTrades = tradeManager->reviewAccount();

        // only open a trade if there is zero
        if (openTrades == 0) {
            // optional is false
            if (auto signal = strategy.decide(tick)) {
                std::string tradeId = tradeManager->openTrade(tick, config.STRATEGY.TRADING_VARIABLES.TRADING_SIZE, *signal);
                std::cout << "Opened trade: " << tradeId << std::endl;
            }
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

        // Strategy-driven management hook. The strategy itself decides
        // whether/when to close positions; the random strategy currently
        // closes every open trade with a small per-tick probability.
        // Dereferencing `tradeManager` (a raw pointer) yields a reference
        // — the parameter type is `TradeManager&`, so `*tradeManager`
        // is what we hand in.
        strategy.during(tickIndex, tick, *tradeManager);

        ++tickIndex;
    }

    std::cout << "Final PnL: " << std::fixed << std::setprecision(2) << tradeManager->calculatePnl() << std::endl;

    // bool closed = tradeManager->closeTrade(tradeId);
    // std::cout << "Trade closed: " << (closed ? "yes" : "no") << std::endl;
}
