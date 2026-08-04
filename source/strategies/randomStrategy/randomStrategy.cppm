// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module randomStrategy;

import std;           // replaces <cstddef>, <optional>, <random>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore (unused here; interface contract)
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager

// A trivial strategy: on every tick it flips a fair coin and returns
// LONG or SHORT. Exits are not the strategy's responsibility — they
// are driven by the stop-loss / take-profit pip distances configured
// on the trade and enforced centrally by Operations. Intended as
// scaffolding for the strategy interface, not as a real trading
// approach.
//
// C# parallels for readers from a C# background:
//   - `class` here is a value/owning type managed via stack or
//     std::unique_ptr — there is no GC. Lifetime is explicit.
//   - `explicit` on a single-arg constructor disables implicit
//     conversion (C# constructors are always explicit, so this is
//     just C++ catching up to the default C# behaviour).
//   - `std::mt19937` is the modern C++ RNG engine; it replaces the
//     globally-shared `std::rand()` used elsewhere in this codebase
//     and gives us the option of seeding for reproducible backtests.
//   - `: public IStrategy` is C++ inheritance syntax. `public` means
//     the inheritance preserves access — outside code can use a
//     `RandomStrategy` anywhere an `IStrategy` is expected. The C#
//     equivalent is `: IStrategy`; C# has no concept of private
//     inheritance, so the `public` keyword has no analogue there.
export class RandomStrategy : public IStrategy {
public:
    explicit RandomStrategy(const tradingDefinitions::StrategyConfig& strategyConfig);

    // Returns `std::nullopt` to mean "no signal — don't trade". The
    // random strategy always returns a direction, but the interface
    // matches future strategies that only fire on certain conditions.
    // `std::optional<T>` is roughly C#'s `Nullable<T>` / `T?` — a
    // value type that may or may not hold a T, with no heap allocation.
    //
    // Not const because the RNG engine mutates its internal state on
    // each call. The `tick` and `barStore` parameters are unused today
    // but keep the interface stable for strategies that look at price
    // or bar history.
    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override;

    // Per-tick management hook. The default RandomStrategy
    // implementation is a no-op — Operations closes trades when
    // their stop-loss or take-profit boundary is hit. `tradeManager`
    // is passed by mutable reference so future strategies (trailing
    // stops, partial closes, scale-ins) can act on open positions
    // here without changing the interface.
    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override;

private:
    tradingDefinitions::StrategyConfig config;
    std::mt19937 rng;
    std::bernoulli_distribution coin;       // fair coin flip for entry direction
    std::bernoulli_distribution closeProb;  // per-tick probability of closing all trades
};

// Member initialiser list (the bit after the `:`) runs in declaration
// order, not the order written here — same caveat called out in
// trade.hpp. The C# equivalent would be field initialisers plus a
// constructor body, but C++ prefers the initialiser list because it
// constructs members directly rather than default-then-assign.
RandomStrategy::RandomStrategy(const tradingDefinitions::StrategyConfig& strategyConfig)
    : config(strategyConfig),
      rng(std::random_device{}()),       // seed once from the OS entropy source
      coin(0.5),
      closeProb(0.0) {}                  // unused — exits are driven by SL/TP in Operations

std::optional<Direction> RandomStrategy::decide(const PriceData& /*tick*/,
                                                const bars::BarStore& /*barStore*/) {
    return coin(rng) ? Direction::LONG : Direction::SHORT;
}

void RandomStrategy::during(const PriceData& /*price*/,
                            const bars::BarStore& /*barStore*/,
                            TradeManager& /*tradeManager*/) {
    // Exits are handled centrally by Operations using each trade's
    // stop-loss / take-profit pip distances. Strategies that want
    // bespoke exit logic (trailing stops, partial closes, etc.)
    // should override this hook.
}
