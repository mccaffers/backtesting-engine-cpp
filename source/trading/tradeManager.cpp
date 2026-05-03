// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "tradeManager.hpp"
#include <atomic>

namespace {
std::string nextTradeId() {
    static std::atomic<uint64_t> counter{0};
    return "T" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}
}

std::string TradeManager::openTrade(const PriceData& tick, boost::decimal::decimal64_t size, Direction direction) {
    auto price = (direction == Direction::LONG) ? tick.ask : tick.bid;
    Trade trade(price, size, direction, tick.symbol);
    trade.id = nextTradeId();
    activeTrades[trade.id] = trade;
    return trade.id;
}

size_t TradeManager::reviewAccount() const {
    return activeTrades.size();
}

bool TradeManager::closeTrade(const std::string& tradeId, boost::decimal::decimal64_t closePrice) {
    auto it = activeTrades.find(tradeId);
    if (it != activeTrades.end()) {
        Trade closed = it->second;
        closed.closePrice = closePrice;
        closed.closeTime = std::chrono::system_clock::now();
        auto diff = closePrice - closed.entryPrice;
        if (closed.direction == Direction::SHORT) diff = -diff;
        // scalingFactor stays an int — boost::decimal has overloads for builtin
        // integer types, so no conversion is needed there.
        closed.pnl = diff * closed.scalingFactor * closed.size;
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

boost::decimal::decimal64_t TradeManager::calculatePnl() const {
    boost::decimal::decimal64_t pnl{0};
    for (const auto& trade : closedTrades) {
        pnl += trade.pnl;
    }
    return pnl;
}


