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
    
    // Private constructor for singleton
    TradeManager() = default;

public:
    static TradeManager* getInstance() {
        if (instance == nullptr) {
            instance = new TradeManager();
        }
        return instance;
    }

    // Add reset method for testing
    static void reset() {
        delete instance;
        instance = nullptr;
        Trade::resetCounter();  // Reset the trade ID counter
    }
    
    // Clear all trades
    void clearAllTrades() {
        activeTrades.clear();
    }

    // Open a new trade
    std::string openTrade(double price, double size, bool isLong) {
        Trade trade(price, size, isLong);
        activeTrades[trade.id] = trade;
        return trade.id;
    }

    // Review account - returns number of open trades
    size_t reviewAccount() const {
        return activeTrades.size();
    }

    // Close a trade
    bool closeTrade(const std::string& tradeId) {
        auto it = activeTrades.find(tradeId);
        if (it != activeTrades.end()) {
            activeTrades.erase(it);
            return true;
        }
        return false;
    }

    // Get active trades
    const std::unordered_map<std::string, Trade>& getActiveTrades() const {
        return activeTrades;
    }
};
