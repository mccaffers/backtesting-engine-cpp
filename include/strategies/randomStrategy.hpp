// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <cstddef>
#include <optional>
#include <random>
#include "models/priceData.hpp"
#include "models/trade.hpp"                       // for Direction enum
#include "strategies/strategy.hpp"                // IStrategy base class
#include "trading_definitions/strategy.hpp"

class TradeManager;  // forward declared in strategy.hpp; redeclaring is harmless

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
class RandomStrategy : public IStrategy {
public:
    explicit RandomStrategy(const trading_definitions::Strategy& strategyConfig);

    // Returns `std::nullopt` to mean "no signal — don't trade". The
    // random strategy always returns a direction, but the interface
    // matches future strategies that only fire on certain conditions.
    // `std::optional<T>` is roughly C#'s `Nullable<T>` / `T?` — a
    // value type that may or may not hold a T, with no heap allocation.
    //
    // Not const because the RNG engine mutates its internal state on
    // each call. The `tick` parameter is unused today but keeps the
    // interface stable for strategies that will look at price.
    std::optional<Direction> decide(const PriceData& tick) override;

    // Per-tick management hook. The default RandomStrategy
    // implementation is a no-op — Operations closes trades when
    // their stop-loss or take-profit boundary is hit. `tradeManager`
    // is passed by mutable reference so future strategies (trailing
    // stops, partial closes, scale-ins) can act on open positions
    // here without changing the interface.
    void during(const PriceData& price,
                TradeManager& tradeManager) override;

private:
    trading_definitions::Strategy config;
    std::mt19937 rng;
    std::bernoulli_distribution coin;       // fair coin flip for entry direction
    std::bernoulli_distribution closeProb;  // per-tick probability of closing all trades
};
