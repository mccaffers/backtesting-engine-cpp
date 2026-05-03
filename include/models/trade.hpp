// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <string_view>
#include <chrono>
#include "symbolScale.hpp"
#include <boost/decimal.hpp>

enum class Direction {
    LONG,
    SHORT
};

struct Trade {
    std::string id;
    double entryPrice;
    double size;
    std::chrono::system_clock::time_point openTime;
    Direction direction;

    std::string dealReference;
    std::string symbol;
    int scalingFactor;
    double stopDistancePips;
    double limitDistancePips;
    std::string strategyId;
    std::string strategyName;

    double closePrice;
    std::chrono::system_clock::time_point closeTime;
    // Realised profit/loss for this trade in pip-points, populated on close.
    // Pip-PnL = price difference * scalingFactor * size (sign flipped for SHORT).
    double pnl;

    // Default constructor
    Trade() : entryPrice(0), size(0), direction(Direction::LONG),
              scalingFactor(0), stopDistancePips(0), limitDistancePips(0),
              closePrice(0), pnl(0),
              openTime(std::chrono::system_clock::now()) {}

    // Copy constructor
    Trade(const Trade& other) = default;

    // Member initializers run in declaration order, not the order written
    // here, so it's safe to derive `scalingFactor` from `tradeSymbol`
    // regardless of where these appear in the list.
    Trade(double price, double quantity, Direction dir, std::string_view tradeSymbol)
        : entryPrice(price),
          size(quantity),
          openTime(std::chrono::system_clock::now()),
          direction(dir),
          symbol(tradeSymbol),
          scalingFactor(symbol_scale::get(tradeSymbol)),
          stopDistancePips(0),
          limitDistancePips(0),
          pnl(0) {
    }

};
