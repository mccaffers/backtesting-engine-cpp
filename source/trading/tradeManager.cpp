// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "tradeManager.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
std::string nextTradeId() {
    static std::atomic<uint64_t> counter{0};
    return "T" + std::to_string(counter.fetch_add(1));
}
}

std::string TradeManager::openTrade(const PriceData& tick,
                                    boost::decimal::decimal64_t size,
                                    Direction direction,
                                    boost::decimal::decimal64_t stopDistancePips,
                                    boost::decimal::decimal64_t limitDistancePips) {
    auto price = (direction == Direction::LONG) ? tick.ask : tick.bid;
    Trade trade(price, size, direction, tick.symbol);
    trade.entryBid = tick.bid;
    trade.entryAsk = tick.ask;
    trade.id = nextTradeId();
    trade.stopDistancePips = stopDistancePips;
    trade.limitDistancePips = limitDistancePips;
    trade.exitReferencePrice = (direction == Direction::LONG) ? tick.bid : tick.ask;
    activeTrades[trade.id] = trade;
    return trade.id;
}

size_t TradeManager::reviewAccount() const {
    return activeTrades.size();
}

bool TradeManager::hasActiveTradeForSymbol(std::string_view symbol) const {
    return std::any_of(activeTrades.begin(), activeTrades.end(),
                       [symbol](const auto& pair) {
                           return pair.second.symbol == symbol;
                       });
}

bool TradeManager::closeTrade(const std::string& tradeId,
                              boost::decimal::decimal64_t closePrice,
                              const PriceData& tick) {
    auto it = activeTrades.find(tradeId);
    if (it != activeTrades.end()) {
        Trade closed = it->second;
        closed.closePrice = closePrice;
        closed.closeTime = tick.timestamp;
        auto diff = closePrice - closed.entryPrice;
        if (closed.direction == Direction::SHORT) diff = -diff;
        // scalingFactor stays an int — boost::decimal has overloads for builtin
        // integer types, so no conversion is needed there.
        closed.pnl = diff * closed.scalingFactor * closed.size;
        closedTrades.push_back(closed);
        activeTrades.erase(it);

        auto t = std::chrono::system_clock::to_time_t(tick.timestamp);
        std::tm utc{};
        gmtime_r(&t, &utc);
        std::ostringstream ts;
        ts << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");

        const char* side = (closed.direction == Direction::LONG) ? "BUY" : "SELL";
        std::cout << ts.str()
                  << ", Trade Closed, " << closed.symbol
                  << ", " << side
                  << ", " << std::showpos << std::fixed << std::setprecision(2) << closed.pnl
                  << std::noshowpos
                  << std::endl;
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


