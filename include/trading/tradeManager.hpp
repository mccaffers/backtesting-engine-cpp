// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <unordered_map>
#include <string_view>
#include <vector>
#include <memory>
#include <boost/decimal.hpp>
#include "trade.hpp"
#include "priceData.hpp"

class TradeManager {
private:
    std::unordered_map<std::string, Trade> activeTrades;
    std::vector<Trade> closedTrades;
    // Running sums maintained by openTrade/markToMarket/closeTrade so the
    // per-tick loss-limit check in runTicks stays O(1): realized PnL across
    // closed trades, and floating (mark-to-market) PnL across open ones.
    boost::decimal::decimal64_t closedPnl{0};
    boost::decimal::decimal64_t openPnl{0};

public:
    TradeManager() = default;
    std::string openTrade(const PriceData& tick,
                          boost::decimal::decimal64_t size,
                          Direction direction,
                          boost::decimal::decimal64_t stopDistancePips = boost::decimal::decimal64_t{0},
                          boost::decimal::decimal64_t limitDistancePips = boost::decimal::decimal64_t{0});
    size_t reviewAccount() const;
    bool hasActiveTradeForSymbol(std::string_view symbol) const;
    // `liquidated` marks the close as forced by the account loss limit
    // rather than earned via SL/TP or strategy logic.
    bool closeTrade(const std::string& tradeId,
                    boost::decimal::decimal64_t closePrice,
                    const PriceData& tick,
                    bool liquidated = false);
    const std::unordered_map<std::string, Trade>& getActiveTrades() const;
    const std::vector<Trade>& getClosedTrades() const;
    // Realized PnL across all closed trades. O(1): returns the running sum.
    boost::decimal::decimal64_t calculatePnl() const;
    // Floating (mark-to-market) PnL across all open trades, as of each
    // trade's last marked price. O(1): returns the running sum.
    boost::decimal::decimal64_t unrealizedPnl() const;
    // Revalue open trades for this tick's symbol at its close-side price
    // (bid for LONG, ask for SHORT), updating their floating PnL.
    void markToMarket(const PriceData& tick);
    // Liquidate every open trade at its last marked price (timestamped with
    // `tick`), realizing the floating PnL — used when a run is cut off.
    void closeAllTrades(const PriceData& tick);
};
