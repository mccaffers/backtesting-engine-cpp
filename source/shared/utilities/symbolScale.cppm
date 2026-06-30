// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// symbolScale — fast symbol -> points-per-pip lookup.
//
// Prices are stored as scaled INT32 "points" in QuestDB (EURUSD 1.10001 ->
// 110001). This returns how many of those integer points make up one pip for
// the symbol: 10 for FX majors and JPY pairs, 100 for indices/commodities,
// 1000 for metals. So a pip distance becomes points via `pips * pointsPerPip`,
// and a point PnL becomes pips via `points / pointsPerPip`.
//
// The engine keeps the per-tick loop entirely in integer points; this factor
// is applied only at the edges (pip distance -> points on trade open, and
// point PnL -> pips at display/reporting). It also doubles as the symbol
// validator: unknown symbols return 0 (kUnknown), which skips the exit check.
//
// Design goals (this is on the hot path of the backtester):
//  - No heap allocation, no hashing, no string parsing at runtime.
//  - The whole table is `constexpr`, so when the caller passes a string
//    literal the compiler can fold the lookup down to a single `mov`.
//  - Exported `inline`/`constexpr` so every importer can inline `get()`.
//
// C# analogy: think of this as a `static readonly Dictionary<string,int>`,
// except the lookup is resolved at compile time when possible.

export module symbolScale;

// std::array<T, N> is a fixed-size, stack-allocated array with an STL-style
// interface; std::string_view is a non-owning (const char* + length) view that
// neither allocates nor copies (closest C# analogues: a fixed buffer and
// ReadOnlySpan<char>).
import std;  // replaces <array>, <string_view>

// `namespace` is C++'s scoping construct. `symbol_scale::get(...)` is
// the fully qualified name from outside this namespace.
export namespace symbol_scale {

// A plain "POD" (plain old data) record. No constructors needed — we use
// brace-initialization below. `string_view` is safe to store here because
// the strings it points to are static string literals with program-long
// lifetime.
//
// Two independent scaling factors live on each entry:
//  - `scale`      : points-per-pip (10 / 100 / 1000), used by the backtester to
//                   convert between integer points and pips (see get()).
//  - `priceScale` : the multiplier that turns a real decimal price into the
//                   scaled INT32 stored in QuestDB (FX majors x100000, JPY pairs
//                   & metals x1000, indices/commodities x100 — see priceData).
//                   Used by the UDP ingest path to scale incoming prices (see
//                   getPriceScale()). It is NOT derivable from `scale` alone: FX
//                   majors and JPY pairs share scale 10 but differ here.
struct Entry {
    std::string_view symbol;
    int scale;
    int priceScale;
};

// Three keywords doing three different jobs on this one declaration:
//  - `inline`    : tells the linker "if you see this defined in multiple
//                  TUs, that's fine — they're all the same thing."
//  - `constexpr` : the value is computable at compile time. The whole
//                  table lives in read-only program memory and the
//                  compiler can use it during constant evaluation.
//  - `std::array<Entry, 29>` : 29 Entry objects laid out contiguously
//                  in memory — great for CPU cache locality during the
//                  binary search below.
//
// IMPORTANT: this table MUST stay sorted ascending by symbol. The
// `static_assert` below enforces that at compile time.
// Columns are {symbol, scale (points-per-pip), priceScale (decimal->INT32
// multiplier)}. scale: FX majors & JPY pairs 10, indices/commodities 100,
// metals 1000. priceScale: FX majors 100000, JPY pairs & metals 1000,
// indices/commodities 100.
inline constexpr std::array<Entry, 29> kTable{{
    {"AUDNZD",          10,  100000},
    {"AUDUSD",          10,  100000},
    {"AUSIDXAUD",      100,     100},
    {"BRENTCMDUSD",    100,     100},
    {"COPPERCMDUSD",   100,     100},
    {"DEUIDXEUR",      100,     100},
    {"EURAUD",          10,  100000},
    {"EURCHF",          10,  100000},
    {"EURGBP",          10,  100000},
    {"EURJPY",          10,    1000},
    {"EURNOK",          10,  100000},
    {"EURUSD",          10,  100000},
    {"FRAIDXEUR",      100,     100},
    {"GBPJPY",          10,    1000},
    {"GBPUSD",          10,  100000},
    {"GBRIDXGBP",      100,     100},
    {"HKGIDXHKD",      100,     100},
    {"JPNIDXJPY",      100,     100},
    {"LIGHTCMDUSD",    100,     100},
    {"NZDUSD",          10,  100000},
    {"USA30IDXUSD",    100,     100},
    {"USA500IDXUSD",   100,     100},
    {"USATECHIDXUSD",  100,     100},
    {"USDCAD",          10,  100000},
    {"USDCHF",          10,  100000},
    {"USDJPY",          10,    1000},
    {"USDSEK",          10,  100000},
    {"XAGUSD",        1000,    1000},
    {"XAUUSD",        1000,    1000},
}};

// Compile-time invariant check. The pattern is an IIFE — Immediately
// Invoked Function Expression. We declare a lambda `[]{ ... }` and
// then invoke it with `()`, all in one expression. This lets us run
// real logic (a loop) inside `static_assert`, which only accepts a
// boolean expression.
//
// If a future maintainer adds an entry in the wrong place, compilation
// fails with the message below — far better than a silently broken
// binary search.
static_assert([] {
    for (std::size_t i = 1; i < kTable.size(); ++i) {
        if (!(kTable[i - 1].symbol < kTable[i].symbol)) return false;
    }
    return true;
}(), "symbol_scale::kTable must be sorted ascending by symbol — binary search depends on it");

// Sentinel returned for unknown symbols. Multiplying a real price by 0
// will produce an obviously-wrong result, which fails loudly rather
// than corrupting silently.
inline constexpr int kUnknown = 0;

// Shared lookup behind get()/getPriceScale(): returns the matching entry or
// nullptr. `constexpr` so the whole search folds to a constant at compile time
// when the symbol is a literal; `noexcept` skips exception bookkeeping at call
// sites. The `std::string_view` parameter (pointer + length, cheap to copy) lets
// callers pass std::string, string literals, or const char* without allocating.
constexpr const Entry* findEntry(std::string_view symbol) noexcept {
    // Standard binary search over the sorted table. [lo, hi) is the half-open
    // range still in play; each iteration halves it (<=5 steps for 29 entries).
    // `lo + ((hi - lo) >> 1)` is the overflow-safe midpoint idiom (`>> 1` is /2).
    std::size_t lo = 0;
    std::size_t hi = kTable.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        const auto& entry = kTable[mid];
        // `string_view::operator<` is a lexicographic (memcmp-like) compare.
        if (entry.symbol < symbol) {
            lo = mid + 1;       // target is in the upper half
        } else if (symbol < entry.symbol) {
            hi = mid;           // target is in the lower half
        } else {
            return &entry;      // exact match
        }
    }
    return nullptr;
}

// Points-per-pip for the symbol (10 / 100 / 1000), or kUnknown if not found.
[[nodiscard]] constexpr int get(std::string_view symbol) noexcept {
    const Entry* entry = findEntry(symbol);
    return entry ? entry->scale : kUnknown;
}

// decimal->INT32 price multiplier (e.g. EURUSD 1.10001 -> 110001), or kUnknown.
// Used by the UDP ingest to scale incoming prices; kUnknown means "drop the
// tick" rather than scale by zero.
[[nodiscard]] constexpr int getPriceScale(std::string_view symbol) noexcept {
    const Entry* entry = findEntry(symbol);
    return entry ? entry->priceScale : kUnknown;
}

} // namespace symbol_scale
