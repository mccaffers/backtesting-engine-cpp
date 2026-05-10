// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <cstddef>
#include <optional>
#include "models/priceData.hpp"
#include "models/trade.hpp"

// Forward declaration. We only refer to `TradeManager` by reference in
// this header, so the compiler doesn't need its full definition here —
// just to know "it's a class". Pulling in the full header would drag
// every TradeManager dependency into every strategy. C# doesn't really
// have an equivalent because it resolves types at the assembly level
// rather than per file, but in C++ this is a standard way to keep
// header dependencies (and compile times) under control.
class TradeManager;

// Abstract base class that every concrete strategy implements.
//
// C# parallels for readers from a C# background:
//   - This is the C++ equivalent of a C# `interface`. C++ has no
//     dedicated `interface` keyword, so you express it as a class
//     whose methods are all pure virtual (the `= 0` suffix). The
//     `I` prefix is borrowed from C# — C++ has no fixed convention,
//     but it's a useful hint because C++ classes routinely mix
//     virtual and concrete methods, so "is this an interface?" isn't
//     always obvious from the keyword alone.
//   - `virtual ~IStrategy() = default;` is mandatory. If you ever
//     delete a derived strategy through an `IStrategy*`, a non-virtual
//     destructor would skip the derived destructor and leak resources.
//     C# handles this for you; in C++ you opt in.
//   - `= 0` makes a method pure virtual, which makes the class
//     abstract — you cannot instantiate `IStrategy` directly, only
//     concrete subclasses. Same behaviour as a C# interface.
//   - Derived classes annotate their implementations with `override`
//     (see `RandomStrategy`). It's optional in C++ but catches
//     signature typos at compile time, exactly like C#'s `override`.
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Entry signal. Returns `std::nullopt` to mean "no trade".
    // Non-const because some implementations (e.g. RandomStrategy)
    // mutate internal RNG state on each call.
    virtual std::optional<Direction> decide(const PriceData& tick) = 0;

    // Called every tick. Receives the TradeManager by mutable
    // reference so strategies can both inspect open positions
    // (`tradeManager.getActiveTrades()`) and act on them
    // (`closeTrade`, future scale/adjust hooks). A reference signals
    // "borrow, don't own" — the strategy must not delete it. The C#
    // analogue is just passing the manager as a parameter; C# has no
    // distinction between reference and pointer so the by-ref nature
    // is implicit there.
    virtual void during(std::size_t tickValue,
                        const PriceData& price,
                        TradeManager& tradeManager) = 0;
};
