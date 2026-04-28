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
#include "tradeManager.hpp"

void Operations::run(const std::vector<PriceData>& ticks) {

    // Loop around every tick
    // Example output:
    // symbol=AUSIDXAUD, ask=8602.4000, bid=8599.4000 timestamp=2026-03-12 18:39:01.076
    // symbol=AUSIDXAUD, ask=8602.9000, bid=8599.9000 timestamp=2026-03-12 18:39:01.584
    // symbol=EURUSD, ask=1.1513, bid=1.1512 timestamp=2026-03-12 18:39:01.644
    // symbol=AUSIDXAUD, ask=8602.4000, bid=8599.4000 timestamp=2026-03-12 18:39:01.770
    // symbol=AUSIDXAUD, ask=8601.9000, bid=8598.9000 timestamp=2026-03-12 18:39:01.982

    auto tradeManager = new TradeManager();

  

        
    for (const auto& tick : ticks) {
        // (void)tick;

        // print first tick
        // auto time_t = std::chrono::system_clock::to_time_t(tick.timestamp);
        // struct tm tm = {};
        // if (localtime_r(&time_t, &tm) == nullptr) {
        //     std::cerr << "Error: failed to convert timestamp" << std::endl;
        // }
        // char buffer[20];
        // std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
        // auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tick.timestamp.time_since_epoch()) % 1000;
        // // printf("symbol=%s, ask=%.4f, bid=%.4f timestamp=%s.%03lld\n", tick.symbol.c_str(), tick.ask, tick.bid, buffer,
        // //        static_cast<long long>(ms.count()));

        size_t openTrades = tradeManager->reviewAccount();
        
        if (openTrades == 0) {
            std::string tradeId = tradeManager->openTrade(tick, 100000, Direction::LONG);
            std::cout << "Opened trade: " << tradeId << std::endl;
        }

        // randomly check account status every 100 ticks
        if (openTrades > 0 && (std::rand() % 100) == 0) {
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

        
        // randomly close trades every 200 ticks
        if (openTrades > 0 && (std::rand() % 200) == 0) {
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
