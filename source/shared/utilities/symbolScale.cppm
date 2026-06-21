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
struct Entry {
    std::string_view symbol;
    int scale;
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
// Values are integer price points per pip (see header comment). FX majors and
// JPY pairs are 10, indices/commodities 100, metals 1000.
inline constexpr std::array<Entry, 29> kTable{{
    {"AUDNZD",          10},
    {"AUDUSD",          10},
    {"AUSIDXAUD",      100},
    {"BRENTCMDUSD",    100},
    {"COPPERCMDUSD",   100},
    {"DEUIDXEUR",      100},
    {"EURAUD",          10},
    {"EURCHF",          10},
    {"EURGBP",          10},
    {"EURJPY",          10},
    {"EURNOK",          10},
    {"EURUSD",          10},
    {"FRAIDXEUR",      100},
    {"GBPJPY",          10},
    {"GBPUSD",          10},
    {"GBRIDXGBP",      100},
    {"HKGIDXHKD",      100},
    {"JPNIDXJPY",      100},
    {"LIGHTCMDUSD",    100},
    {"NZDUSD",          10},
    {"USA30IDXUSD",    100},
    {"USA500IDXUSD",   100},
    {"USATECHIDXUSD",  100},
    {"USDCAD",          10},
    {"USDCHF",          10},
    {"USDJPY",          10},
    {"USDSEK",          10},
    {"XAGUSD",        1000},
    {"XAUUSD",        1000},
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

// `[[nodiscard]]` : compiler warning if the caller ignores the returned
//                   value (this function has no other purpose, so
//                   ignoring the result is almost always a bug).
// `constexpr`     : callable at compile time. When the symbol is a
//                   literal known to the compiler, the entire binary
//                   search is folded away and the result becomes a
//                   constant in the generated assembly.
// `noexcept`      : promises this function will not throw. Lets the
//                   compiler skip exception-handling bookkeeping at
//                   call sites.
// Parameter is `std::string_view` (by value — it's just a pointer +
// length, cheap to copy) so callers can pass `std::string`, string
// literals, or `const char*` without converting or allocating.
[[nodiscard]] constexpr int get(std::string_view symbol) noexcept {
    // Standard binary search over the sorted table.
    // `lo` and `hi` are the half-open range [lo, hi) of indices still
    // in play. Each iteration halves the range, so for 29 entries we
    // do at most 5 iterations.
    std::size_t lo = 0;
    std::size_t hi = kTable.size();
    while (lo < hi) {
        // `lo + ((hi - lo) >> 1)` is the overflow-safe way to compute
        // the midpoint. `(lo + hi) / 2` would be wrong if the indices
        // were near std::size_t's max; not a real risk here, but it's
        // the canonical idiom worth learning. `>> 1` is just `/ 2`.
        const std::size_t mid = lo + ((hi - lo) >> 1);
        const auto& entry = kTable[mid];

        // Three-way compare on the symbol. `string_view::operator<`
        // does a lexicographic comparison (essentially memcmp).
        if (entry.symbol < symbol) {
            lo = mid + 1;       // target is in the upper half
        } else if (symbol < entry.symbol) {
            hi = mid;           // target is in the lower half
        } else {
            return entry.scale; // exact match
        }
    }
    return kUnknown;
}

} // namespace symbol_scale
