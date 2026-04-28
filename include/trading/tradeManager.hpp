// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <unordered_map>
#include <vector>
#include <memory>
#include "trade.hpp"
#include "priceData.hpp"

class TradeManager {
private:
    std::unordered_map<std::string, Trade> activeTrades;
    std::vector<Trade> closedTrades;

public:
    TradeManager() = default;
    std::string openTrade(const PriceData& tick, double size, Direction direction);
    size_t reviewAccount() const;
    bool closeTrade(const std::string& tradeId, double closePrice);
    const std::unordered_map<std::string, Trade>& getActiveTrades() const;
    const std::vector<Trade>& getClosedTrades() const;
    double calculatePnl() const;
};
