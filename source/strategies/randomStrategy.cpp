// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "strategies/randomStrategy.hpp"

// Member initialiser list (the bit after the `:`) runs in declaration
// order, not the order written here — same caveat called out in
// trade.hpp. The C# equivalent would be field initialisers plus a
// constructor body, but C++ prefers the initialiser list because it
// constructs members directly rather than default-then-assign.
RandomStrategy::RandomStrategy(const trading_definitions::Strategy& strategyConfig)
    : config(strategyConfig),
      rng(std::random_device{}()),   // seed once from the OS entropy source
      coin(0.5) {}

std::optional<Direction> RandomStrategy::decide(const PriceData& /*tick*/) {
    return coin(rng) ? Direction::LONG : Direction::SHORT;
}
