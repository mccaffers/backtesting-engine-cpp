// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "strategies/randomStrategy.hpp"

#include <iostream>
#include <string>
#include <vector>

#include "tradeManager.hpp"

// Member initialiser list (the bit after the `:`) runs in declaration
// order, not the order written here — same caveat called out in
// trade.hpp. The C# equivalent would be field initialisers plus a
// constructor body, but C++ prefers the initialiser list because it
// constructs members directly rather than default-then-assign.
RandomStrategy::RandomStrategy(const trading_definitions::Strategy& strategyConfig)
    : config(strategyConfig),
      rng(std::random_device{}()),       // seed once from the OS entropy source
      coin(0.5),
      closeProb(1.0 / 200.0) {}          // ~0.5% chance per tick — same odds as the
                                         // old `std::rand() % 200 == 0` in operations.cpp

std::optional<Direction> RandomStrategy::decide(const PriceData& /*tick*/) {
    return coin(rng) ? Direction::LONG : Direction::SHORT;
}

void RandomStrategy::during(std::size_t /*tickValue*/,
                            const PriceData& price,
                            TradeManager& tradeManager) {
    const auto& openTrades = tradeManager.getActiveTrades();
    if (openTrades.empty()) return;
    if (!closeProb(rng)) return;

    // Two-phase pattern: collect IDs first, then close. Closing inside
    // the range-for loop would invalidate the iterator we're walking,
    // because `closeTrade` erases the entry from the underlying map.
    // Same trap as mutating a C# Dictionary while iterating it.
    std::vector<std::string> idsToClose;
    idsToClose.reserve(openTrades.size());
    for (const auto& [id, trade] : openTrades) {
        idsToClose.push_back(id);
    }
    for (const auto& id : idsToClose) {
        bool closed = tradeManager.closeTrade(id, price.bid);
        std::cout << "Closed trade ID: " << id << " - " << (closed ? "success" : "failure") << std::endl;
    }
}
