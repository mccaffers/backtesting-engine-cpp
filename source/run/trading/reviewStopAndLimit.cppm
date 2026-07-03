// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module reviewStopAndLimit;

import std;                // replaces <cstdint>
import tradeManager;       // TradeManager
import trade;              // Trade
import exitRules;          // trading::exit_rules::checkExit
import priceData;          // PriceData

export namespace trading {

// Close the tick symbol's open trade if its SL/TP has been hit on this tick.
// TradeManager keys open trades by symbol (at most one per symbol), so this
// is a single O(1) lookup plus checkExit's integer comparisons — nothing
// allocates on the per-tick path.
//
// Symbol-keying is also what keeps the check symbol-safe: a tick for one
// instrument can never reach a trade on another. `exit_rules::checkExit` is
// symbol-agnostic and would otherwise compare e.g. an AUSIDXAUD price
// (thousands) against a EURUSD stop level (~1.10) and spuriously close the
// trade.
inline void reviewStopAndLimit(TradeManager& tradeManager, const PriceData& tick) {
    const Trade* trade = tradeManager.findActiveTrade(tick.symbol);
    if (trade == nullptr) return;
    if (auto exitPrice = trading::exit_rules::checkExit(*trade, tick)) {
        tradeManager.closeTrade(tick.symbol, *exitPrice, tick);
    }
}

} // namespace trading
