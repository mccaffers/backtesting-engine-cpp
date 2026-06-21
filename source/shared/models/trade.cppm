// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module trade;

import std;          // replaces <cstdint>, <string>, <string_view>, <chrono>
import symbolScale;  // symbol_scale::get for the scaling factor

export enum class Direction {
    LONG,
    SHORT
};

export struct Trade {
    std::string id;
    // Everything is integer: prices/exit-prices are scaled INT32 points (see
    // the priceData / symbolScale modules), distances are whole pips, size is
    // whole lots, and PnL is int64 points-per-lot. Pips are derived only at the
    // display/reporting boundary (points / scalingFactor).
    std::int32_t entryPrice;
    std::int32_t entryBid;
    std::int32_t entryAsk;
    std::int32_t size;
    std::chrono::system_clock::time_point openTime;
    Direction direction;

    std::string dealReference;
    std::string symbol;
    int scalingFactor;
    // SL/TP distances in integer price points (same scale as prices); a value
    // of 0 means that leg is disarmed.
    std::int32_t stopDistancePips;
    std::int32_t limitDistancePips;
    std::int32_t exitReferencePrice;
    // Exit trigger prices, precomputed once in TradeManager::openTrade from
    // exitReferencePrice +/- the distances, so the per-tick exit check is a pure
    // integer comparison. Only meaningful when the matching distance is non-zero.
    std::int32_t stopPrice;
    std::int32_t limitPrice;
    std::string strategyId;
    std::string strategyName;

    std::int32_t closePrice = 0;
    std::chrono::system_clock::time_point closeTime;
    // Realised profit/loss in int64 points-per-lot, populated on close:
    // PnL = (closePrice - entryPrice) * size (sign flipped for SHORT). Divide by
    // scalingFactor (points-per-pip) at the reporting boundary to get pips.
    std::int64_t pnl;

    // Mark-to-market state while the trade is open, maintained by
    // TradeManager: the most recent close-side price seen for this symbol and
    // the floating PnL at that price. Feeds the account loss limit, and
    // lastMarkPrice is the liquidation price if the run is cut off.
    // floatingPnl is zeroed on close — its value has been realized into pnl.
    std::int32_t lastMarkPrice;
    std::int64_t floatingPnl;

    // True when the close was forced by the account loss limit (liquidation
    // at the last marked price) rather than earned via SL/TP or strategy
    // logic — lets reporting separate forced closes from organic ones.
    bool liquidated = false;

    // Default constructor
    Trade() : entryPrice(0), entryBid(0), entryAsk(0), size(0), direction(Direction::LONG),
              scalingFactor(0), stopDistancePips(0), limitDistancePips(0),
              exitReferencePrice(0), stopPrice(0), limitPrice(0),
              closePrice(0), pnl(0),
              lastMarkPrice(0), floatingPnl(0),
              openTime(std::chrono::system_clock::now()) {}

    // Copy constructor
    Trade(const Trade& other) = default;

    // Member initializers run in declaration order, not the order written
    // here, so it's safe to derive `scalingFactor` from `tradeSymbol`
    // regardless of where these appear in the list.
    Trade(std::int32_t price, std::int32_t quantity, Direction dir, std::string_view tradeSymbol)
        : entryPrice(price),
          entryBid(0),
          entryAsk(0),
          size(quantity),
          openTime(std::chrono::system_clock::now()),
          direction(dir),
          symbol(tradeSymbol),
          scalingFactor(symbol_scale::get(tradeSymbol)),
          stopDistancePips(0),
          limitDistancePips(0),
          exitReferencePrice(0),
          stopPrice(0),
          limitPrice(0),
          pnl(0),
          lastMarkPrice(0),
          floatingPnl(0) {
    }

};
