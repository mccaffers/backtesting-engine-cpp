// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <ctime>  // POSIX gmtime_r (not exported by `import std`)

#include "shared/utilities/backtestLog.hpp"

export module tradeManager;

import std;        // replaces <cstdint>, <unordered_map>, <string_view>, <vector>,
                   // <memory>, <algorithm>, <atomic>, <chrono>, <format>, <iomanip>,
                   // <iostream>, <sstream>
import trade;      // Trade, Direction
import priceData;  // PriceData

export class TradeManager {
private:
    std::unordered_map<std::string, Trade> activeTrades;
    std::vector<Trade> closedTrades;
    // Running sums (int64 points-per-lot) maintained by openTrade/markToMarket/
    // closeTrade so the per-tick loss-limit check in runTicks stays O(1):
    // realized PnL across closed trades, and floating (mark-to-market) PnL
    // across open ones.
    std::int64_t closedPnl{0};
    std::int64_t openPnl{0};

public:
    TradeManager() = default;
    std::string openTrade(const PriceData& tick,
                          std::int32_t size,
                          Direction direction,
                          std::int32_t stopDistancePips = 0,
                          std::int32_t limitDistancePips = 0);
    std::size_t reviewAccount() const;
    bool hasActiveTradeForSymbol(std::string_view symbol) const;
    // `liquidated` marks the close as forced by the account loss limit
    // rather than earned via SL/TP or strategy logic.
    bool closeTrade(const std::string& tradeId,
                    std::int32_t closePrice,
                    const PriceData& tick,
                    bool liquidated = false);
    const std::unordered_map<std::string, Trade>& getActiveTrades() const;
    const std::vector<Trade>& getClosedTrades() const;
    // Realized PnL (int64 points-per-lot) across all closed trades. O(1).
    std::int64_t calculatePnl() const;
    // Floating (mark-to-market) PnL (int64 points-per-lot) across all open
    // trades, as of each trade's last marked price. O(1).
    std::int64_t unrealizedPnl() const;
    // Revalue open trades for this tick's symbol at its close-side price
    // (bid for LONG, ask for SHORT), updating their floating PnL.
    void markToMarket(const PriceData& tick);
    // Liquidate every open trade at its last marked price (timestamped with
    // `tick`), realizing the floating PnL — used when a run is cut off.
    void closeAllTrades(const PriceData& tick);
};

namespace {
std::string nextTradeId() {
    static std::atomic<std::uint64_t> counter{0};
    return std::format("T{}", counter.fetch_add(1));
}

// Floating PnL (int64 points-per-lot) of an open trade valued at `mark` — the
// same formula closeTrade uses for realized PnL, so liquidating at the last
// mark realizes exactly the floating amount. Pure integer: no decimal on the
// per-tick path. Divide by scalingFactor for pips at the reporting boundary.
std::int64_t floatingPnlAt(const Trade& trade, std::int32_t mark) {
    std::int32_t diff = mark - trade.entryPrice;
    if (trade.direction == Direction::SHORT) diff = -diff;
    return static_cast<std::int64_t>(diff) * trade.size;
}
}

std::string TradeManager::openTrade(const PriceData& tick,
                                    std::int32_t size,
                                    Direction direction,
                                    std::int32_t stopDistancePips,
                                    std::int32_t limitDistancePips) {
    auto price = (direction == Direction::LONG) ? tick.ask : tick.bid;
    Trade trade(price, size, direction, tick.symbol);
    trade.entryBid = tick.bid;
    trade.entryAsk = tick.ask;
    trade.id = nextTradeId();
    trade.stopDistancePips = stopDistancePips;
    trade.limitDistancePips = limitDistancePips;
    trade.exitReferencePrice = (direction == Direction::LONG) ? tick.bid : tick.ask;
    // Precompute the SL/TP trigger prices once, so the per-tick exit check is a
    // pure integer comparison. Distances are in pips; convert to stored points
    // here (pips * points-per-pip) and anchor on exitReferencePrice (the
    // close-side of the entry spread).
    const std::int32_t stopOffset  = stopDistancePips  * trade.scalingFactor;
    const std::int32_t limitOffset = limitDistancePips * trade.scalingFactor;
    if (direction == Direction::LONG) {
        trade.stopPrice  = trade.exitReferencePrice - stopOffset;
        trade.limitPrice = trade.exitReferencePrice + limitOffset;
    } else {
        trade.stopPrice  = trade.exitReferencePrice + stopOffset;
        trade.limitPrice = trade.exitReferencePrice - limitOffset;
    }
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
    std::vector<std::pair<std::string, std::int32_t>> toClose;
    toClose.reserve(activeTrades.size());
    for (const auto& [id, trade] : activeTrades) {
        toClose.emplace_back(id, trade.lastMarkPrice);
    }
    for (const auto& [id, price] : toClose) {
        closeTrade(id, price, tick, /*liquidated=*/true);
    }
}

std::size_t TradeManager::reviewAccount() const {
    return activeTrades.size();
}

bool TradeManager::hasActiveTradeForSymbol(std::string_view symbol) const {
    return std::any_of(activeTrades.begin(), activeTrades.end(),
                       [symbol](const auto& pair) {
                           return pair.second.symbol == symbol;
                       });
}

bool TradeManager::closeTrade(const std::string& tradeId,
                              std::int32_t closePrice,
                              const PriceData& tick,
                              bool liquidated) {
    auto it = activeTrades.find(tradeId);
    if (it != activeTrades.end()) {
        Trade closed = it->second;
        closed.closePrice = closePrice;
        closed.closeTime = tick.timestamp;
        closed.liquidated = liquidated;
        std::int32_t diff = closePrice - closed.entryPrice;
        if (closed.direction == Direction::SHORT) diff = -diff;
        // Realized PnL in int64 points-per-lot (converted to pips at reporting).
        closed.pnl = static_cast<std::int64_t>(diff) * closed.size;
        closedPnl += closed.pnl;
        openPnl -= it->second.floatingPnl;  // realized now, no longer floating
        closed.floatingPnl = 0;
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
            // PnL is stored in points-per-lot; show it in pips for readability.
            const double pnlPips = closed.scalingFactor != 0
                ? static_cast<double>(closed.pnl) / closed.scalingFactor
                : 0.0;
            std::cout << ts.str()
                      << ", Trade Closed, " << closed.symbol
                      << ", " << side
                      << ", " << std::showpos << std::fixed << std::setprecision(2) << pnlPips
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

std::int64_t TradeManager::calculatePnl() const {
    return closedPnl;
}

std::int64_t TradeManager::unrealizedPnl() const {
    return openPnl;
}
