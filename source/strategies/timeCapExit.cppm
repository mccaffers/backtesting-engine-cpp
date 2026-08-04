// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module timeCapExit;

import std;           // replaces <chrono>, <cstdint>
import priceData;     // PriceData
import trade;         // Trade, Direction
import tradeManager;  // TradeManager

export namespace strategy_exits {

// The MAX_TRADE_DURATION_MINUTES exit shared by every strategy's during():
// close the tick symbol's open trade once it has been open STRICTLY longer
// than `cap` (a trade exactly at the cap stays open), at the exit-side
// price — a LONG closes at the bid, a SHORT at the ask, matching
// exit_rules::checkExit. cap <= 0 disables the exit. Returns true when it
// closed the trade so callers with further exit logic (e.g. RangeVelocity's
// opposite-run exit) can stop managing a position that no longer exists.
inline bool closeIfPastCap(const PriceData& price, TradeManager& tradeManager,
                           std::chrono::minutes cap) {
    if (cap <= std::chrono::minutes::zero()) {
        return false;
    }
    const Trade* trade = tradeManager.findActiveTrade(price.symbol);
    if (trade == nullptr || price.timestamp - trade->openTime <= cap) {
        return false;
    }
    // Read the direction BEFORE closeTrade — closing erases the map node the
    // pointer aims into.
    const std::int32_t closePrice =
        trade->direction == Direction::LONG ? price.bid : price.ask;
    tradeManager.closeTrade(price.symbol, closePrice, price);
    return true;
}

}  // namespace strategy_exits
