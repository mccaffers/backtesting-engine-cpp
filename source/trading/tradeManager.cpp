// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "tradeManager.hpp"

std::string TradeManager::openTrade(const PriceData& tick, double size, Direction direction) {
    double price = (direction == Direction::LONG) ? tick.ask : tick.bid;
    Trade trade(price, size, direction);
    activeTrades[trade.id] = trade;
    return trade.id;
}

size_t TradeManager::reviewAccount() const {
    return activeTrades.size();
}

bool TradeManager::closeTrade(const std::string& tradeId, double closePrice) {
    auto it = activeTrades.find(tradeId);
    if (it != activeTrades.end()) {
        Trade closed = it->second;
        closed.closePrice = closePrice;
        closed.closeTime = std::chrono::system_clock::now();
        closedTrades.push_back(closed);
        activeTrades.erase(it);
        return true;
    }
    return false;
}

const std::unordered_map<std::string, Trade>& TradeManager::getActiveTrades() const {
    return activeTrades;
}

const std::vector<Trade>& TradeManager::getClosedTrades() const {
    return closedTrades;
}

double TradeManager::calculatePnl() const {
    double pnl = 0.0;
    for (const auto& trade : closedTrades) {
        double diff = trade.closePrice - trade.entryPrice;
        if (trade.direction == Direction::SHORT) diff = -diff;
        pnl += diff * trade.size;
    }
    return pnl;
}


