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
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "backtestLog.hpp"

namespace {
std::string nextTradeId() {
    static std::atomic<uint64_t> counter{0};
    return std::format("T{}", counter.fetch_add(1));
}

// Floating PnL of an open trade valued at `mark` — the same formula
// closeTrade uses for realized PnL, so liquidating at the last mark realizes
// exactly the floating amount.
boost::decimal::decimal64_t floatingPnlAt(const Trade& trade,
                                          boost::decimal::decimal64_t mark) {
    auto diff = mark - trade.entryPrice;
    if (trade.direction == Direction::SHORT) diff = -diff;
    return diff * trade.scalingFactor * trade.size;
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
    // Mark the trade at its entry tick: the close side of the spread. The
    // initial floating PnL is therefore the spread cost — true mark-to-market
    // equity dips by the spread the moment a trade opens.
    trade.lastMarkPrice = trade.exitReferencePrice;
    trade.floatingPnl = floatingPnlAt(trade, trade.lastMarkPrice);
    openPnl += trade.floatingPnl;
    activeTrades[trade.id] = trade;
    return trade.id;
}

void TradeManager::markToMarket(const PriceData& tick) {
    for (auto& [id, trade] : activeTrades) {
        if (trade.symbol != tick.symbol) continue;
        const auto mark = (trade.direction == Direction::LONG) ? tick.bid : tick.ask;
        const auto updated = floatingPnlAt(trade, mark);
        openPnl += updated - trade.floatingPnl;
        trade.floatingPnl = updated;
        trade.lastMarkPrice = mark;
    }
}

void TradeManager::closeAllTrades(const PriceData& tick) {
    // Snapshot ids/prices first: closeTrade mutates activeTrades.
    std::vector<std::pair<std::string, boost::decimal::decimal64_t>> toClose;
    toClose.reserve(activeTrades.size());
    for (const auto& [id, trade] : activeTrades) {
        toClose.emplace_back(id, trade.lastMarkPrice);
    }
    for (const auto& [id, price] : toClose) {
        closeTrade(id, price, tick, /*liquidated=*/true);
    }
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
                              const PriceData& tick,
                              bool liquidated) {
    auto it = activeTrades.find(tradeId);
    if (it != activeTrades.end()) {
        Trade closed = it->second;
        closed.closePrice = closePrice;
        closed.closeTime = tick.timestamp;
        closed.liquidated = liquidated;
        auto diff = closePrice - closed.entryPrice;
        if (closed.direction == Direction::SHORT) diff = -diff;
        // scalingFactor stays an int — boost::decimal has overloads for builtin
        // integer types, so no conversion is needed there.
        closed.pnl = diff * closed.scalingFactor * closed.size;
        closedPnl += closed.pnl;
        openPnl -= it->second.floatingPnl;  // realized now, no longer floating
        closed.floatingPnl = boost::decimal::decimal64_t{0};
        closedTrades.push_back(closed);
        activeTrades.erase(it);

        // Per-trade chatter is skipped under concurrent backtests (quiet),
        // which also avoids the formatting work below.
        if (!backtest_log::quiet) {
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
        }
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
    return closedPnl;
}

boost::decimal::decimal64_t TradeManager::unrealizedPnl() const {
    return openPnl;
}


