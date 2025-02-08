// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <unordered_map>
#include <memory>
#include "trade.hpp"

class TradeManager {
private:
    static TradeManager* instance;
    std::unordered_map<std::string, Trade> activeTrades;
    
    TradeManager() = default;

public:
    static TradeManager* getInstance();
    static void reset();
    void clearAllTrades();
    std::string openTrade(double price, double size, bool isLong);
    size_t reviewAccount() const;
    bool closeTrade(const std::string& tradeId);
    const std::unordered_map<std::string, Trade>& getActiveTrades() const;
};
