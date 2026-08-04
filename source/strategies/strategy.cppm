// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module strategy;

import std;           // replaces <optional>, <cstddef>
import barStore;      // bars::BarStore — the shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager

// IStrategy's interface refers to TradeManager only by reference, but once
// TradeManager is module-attached it cannot be forward-declared in the global
// module (that would be a distinct, incompatible type and RandomStrategy's
// override would stop matching), so the strategy module imports it directly.

// Abstract base class that every concrete strategy implements.
//
// C# parallels for readers from a C# background:
//   - This is the C++ equivalent of a C# `interface`. C++ has no
//     dedicated `interface` keyword, so you express it as a class
//     whose methods are all pure virtual (the `= 0` suffix). The
//     `I` prefix is borrowed from C#, C++ has no fixed convention,
//     but it's a useful hint because C++ classes routinely mix
//     virtual and concrete methods, so "is this an interface?" isn't
//     always obvious from the keyword alone.
//   - `virtual ~IStrategy() = default;` is mandatory. If you ever
//     delete a derived strategy through an `IStrategy*`, a non-virtual
//     destructor would skip the derived destructor and leak resources.
//     C# handles this for you; in C++ you opt in.
//   - `= 0` makes a method pure virtual, which makes the class
//     abstract, you cannot instantiate `IStrategy` directly, only
//     concrete subclasses. Same behaviour as a C# interface.
//   - Derived classes annotate their implementations with `override`
//     (see `RandomStrategy`). It's optional in C++ but catches
//     signature typos at compile time, exactly like C#'s `override`.
export class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Entry signal. Returns `std::nullopt` to mean "no trade".
    // Non-const because some implementations (e.g. RandomStrategy)
    // mutate internal RNG state on each call.
    //
    // `barStore` is the loop owner's shared bar pipeline (runLoop / a live
    // worker), updated once per tick BEFORE this call — so decide() sees
    // bar state that already includes the tick it is judging (the
    // in-progress bar's close IS this tick). Strategies own no bar state of
    // their own — the store is the single pipeline shared with the
    // pre-decide ATR entry conditions.
    virtual std::optional<Direction> decide(const PriceData& tick,
                                            const bars::BarStore& barStore) = 0;

    // Called every tick. Receives the TradeManager by mutable
    // reference so strategies can both inspect open positions
    // (`tradeManager.getActiveTrades()`, keyed by symbol) and act on
    // them (`closeTrade(symbol, ...)`, future scale/adjust hooks). A
    // reference signals
    // "borrow, don't own", the strategy must not delete it. The C#
    // analogue is just passing the manager as a parameter; C# has no
    // distinction between reference and pointer so the by-ref nature
    // is implicit there.
    // `barStore` is the same shared pipeline decide() sees — passed here
    // because bar-based exit logic cannot live in decide(): the run loop
    // gates decide() behind "no open trade for this symbol", so a strategy
    // managing an open position only ever observes bars from this hook (and
    // a live position can be a broker-seeded trade decide() never saw).
    virtual void during(const PriceData& price,
                        const bars::BarStore& barStore,
                        TradeManager& tradeManager) = 0;
};
