// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <utility>
#include <vector>
#include <boost/decimal.hpp>
#include "tradeManager.hpp"
#include "exitRules.hpp"
#include "models/priceData.hpp"

namespace trading {

// Walk every active trade, close any whose SL/TP has been hit on this
// tick. Two-phase to avoid invalidating the map iterator while erasing.
//
// Symbol-aware: a tick for one instrument must never trigger an exit on
// a trade opened for a different instrument. The per-trade filter below
// is the production fix — `exit_rules::checkExit` is symbol-agnostic
// and would otherwise compare e.g. an AUSIDXAUD price (thousands)
// against a EURUSD stop level (~1.10) and spuriously close the trade.
inline void reviewStopAndLimit(TradeManager& tradeManager, const PriceData& tick) {
    const auto& openTrades = tradeManager.getActiveTrades();
    if (openTrades.empty()) return;

    std::vector<std::pair<std::string, boost::decimal::decimal64_t>> toClose;
    toClose.reserve(openTrades.size());
    for (const auto& [id, trade] : openTrades) {
        if (trade.symbol != tick.symbol) continue;
        if (auto exitPrice = trading::exit_rules::checkExit(trade, tick)) {
            toClose.emplace_back(id, *exitPrice);
        }
    }
    for (const auto& [id, exitPrice] : toClose) {
        tradeManager.closeTrade(id, exitPrice, tick);
    }
}

} // namespace trading
