// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "tradeManager.hpp"

TradeManager* TradeManager::instance = nullptr;

TradeManager* TradeManager::getInstance() {
    if (instance == nullptr) {
        instance = new TradeManager();
    }
    return instance;
}

void TradeManager::reset() {
    delete instance;
    instance = nullptr;
    Trade::resetCounter();
}

void TradeManager::clearAllTrades() {
    activeTrades.clear();
}

std::string TradeManager::openTrade(double price, double size, bool isLong) {
    Trade trade(price, size, isLong);
    activeTrades[trade.id] = trade;
    return trade.id;
}

size_t TradeManager::reviewAccount() const {
    return activeTrades.size();
}

bool TradeManager::closeTrade(const std::string& tradeId) {
    auto it = activeTrades.find(tradeId);
    if (it != activeTrades.end()) {
        activeTrades.erase(it);
        return true;
    }
    return false;
}

const std::unordered_map<std::string, Trade>& TradeManager::getActiveTrades() const {
    return activeTrades;
}
