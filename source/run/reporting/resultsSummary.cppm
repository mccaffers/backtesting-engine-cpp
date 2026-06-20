// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/decimal.hpp>

#include "shared/utilities/backtestLog.hpp"
#include "run/reporting/tradingResults.hpp"

export module resultsSummary;

import std;           // replaces <iostream>, <iomanip>, <cstddef>
import tradeManager;  // TradeManager
import trade;         // Trade, Direction

export class ResultsSummary {
public:
    static TradingResultsStats collect(const TradeManager& tradeManager);
    static void summarise(const TradeManager& tradeManager);
};

TradingResultsStats ResultsSummary::collect(const TradeManager& tradeManager) {
    const auto& activeTrades = tradeManager.getActiveTrades();
    const auto& closedTrades = tradeManager.getClosedTrades();

    const std::size_t openedCount = activeTrades.size() + closedTrades.size();
    const std::size_t closedCount = closedTrades.size();

    std::size_t openedLong = 0;
    std::size_t openedShort = 0;
    for (const auto& [id, trade] : activeTrades) {
        if (trade.direction == Direction::LONG) ++openedLong;
        else ++openedShort;
    }

    std::size_t closedLong = 0;
    std::size_t closedShort = 0;
    std::size_t winners = 0;
    std::size_t losers = 0;
    std::size_t breakeven = 0;
    std::size_t liquidated = 0;
    boost::decimal::decimal64_t pnlSum{0};   // accumulated in pips
    for (const auto& trade : closedTrades) {
        if (trade.direction == Direction::LONG) ++closedLong;
        else ++closedShort;
        if (trade.pnl > 0) ++winners;
        else if (trade.pnl < 0) ++losers;
        else ++breakeven;
        if (trade.liquidated) ++liquidated;
        // trade.pnl is int64 points-per-lot; convert to pips using the trade's
        // own points-per-pip so mixed-symbol runs sum correctly.
        if (trade.scalingFactor != 0) {
            pnlSum += boost::decimal::decimal64_t{trade.pnl} / trade.scalingFactor;
        }
    }
    openedLong  += closedLong;
    openedShort += closedShort;

    TradingResultsStats stats;
    stats.finalPnl     = pnlSum;
    stats.tradesOpened = openedCount;
    stats.tradesClosed = closedCount;
    stats.openedLong   = openedLong;
    stats.openedShort  = openedShort;
    stats.closedLong   = closedLong;
    stats.closedShort  = closedShort;
    stats.winners      = winners;
    stats.losers       = losers;
    stats.breakeven    = breakeven;
    stats.liquidated   = liquidated;
    if (closedCount == 0) {
        stats.avgPnl = std::nullopt;
    } else {
        stats.avgPnl = pnlSum / boost::decimal::decimal64_t{static_cast<long long>(closedCount)};
    }
    return stats;
}

void ResultsSummary::summarise(const TradeManager& tradeManager) {
    // Per-strategy summary is skipped under concurrent backtests (quiet).
    if (backtest_log::quiet) {
        return;
    }

    const auto stats = collect(tradeManager);

    std::cout << "Final PnL: " << std::fixed << std::setprecision(2) << stats.finalPnl << std::endl;
    std::cout << "Trades opened: " << stats.tradesOpened
              << "  (LONG: " << stats.openedLong << ", SHORT: " << stats.openedShort << ")" << std::endl;
    std::cout << "Trades closed: " << stats.tradesClosed
              << "  (LONG: " << stats.closedLong << ", SHORT: " << stats.closedShort << ")" << std::endl;
    std::cout << "Winners: " << stats.winners
              << "   Losers: " << stats.losers
              << "   Breakeven: " << stats.breakeven << std::endl;
    if (!stats.avgPnl) {
        std::cout << "Average PnL per closed trade: n/a (0 closed)" << std::endl;
    } else {
        std::cout << "Average PnL per closed trade: "
                  << std::fixed << std::setprecision(2) << *stats.avgPnl << std::endl;
    }
}
