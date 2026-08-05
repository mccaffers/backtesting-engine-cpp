// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// marketHours — the peak-market-hours ENTRY filter (a port of the C# engine's
// MarketHours.TradePermitted). Every symbol maps to the session whose open
// drives its liquidity; tradePermitted answers whether a NEW ENTRY is allowed
// at a UTC instant. It gates entries only — exits (SL/TP, liquidation, the
// live book sync and during()) must never consult it.
//
// Rules, all in UTC (tick timestamps are UTC end-to-end, see tickPacket):
//   - Weekend / low-liquidity block for every session: Friday from 16:00,
//     all of Sunday, Monday before 02:00.
//   - Asia:    00:00-06:00, fixed — no DST anchor.
//   - Europe:  three hours from the London open — 07:00 under BST, else 08:00.
//   - NewYork: three hours from the US open — 13:30 under EDT, else 14:30.
//   - Unknown symbol: blocked. Fail-closed on purpose (the C# behaviour, and
//     the trade-lock doctrine): a missed entry is recoverable, an entry traded
//     in the wrong session is not. The table below must partition the same
//     29-symbol universe as symbol_scale::kTable (pinned by test).
//
// DST is hand-rolled rather than tzdb-based: libc++'s chrono time-zone
// database support differs between the CI clang and the local Homebrew clang,
// while the calendar types used here are plain C++20 chrono, portable and
// constexpr. Date-level granularity ("is this DAY in summer time?") is exact
// for this filter because both markets transition in the small hours of a
// Sunday — and every Sunday is already weekend-blocked outright.

export module marketHours;

import std;  // replaces <array>, <chrono>, <cstddef>, <string_view>

export namespace market_hours {

// Which market's open anchors the symbol's peak trading window.
enum class Session {
    Asia,     // 00:00-06:00 UTC, fixed
    Europe,   // three hours from the London open (BST-aware)
    NewYork,  // three hours from the US open (EDT-aware)
    Unknown,  // not in kTable — tradePermitted fails closed
};

struct Entry {
    std::string_view symbol;
    Session session;
};

// MUST stay sorted ascending by symbol (static_assert below) — sessionFor
// binary-searches it. Same universe as symbol_scale::kTable / live::kMarkets;
// tests/marketHours.cpp pins the cross-table equivalence both ways, so adding
// a symbol to one table without the others fails at test time.
inline constexpr std::array<Entry, 29> kTable{{
    {"AUDNZD", Session::Asia},
    {"AUDUSD", Session::Asia},
    {"AUSIDXAUD", Session::Asia},
    {"BRENTCMDUSD", Session::NewYork},
    {"COPPERCMDUSD", Session::NewYork},
    {"DEUIDXEUR", Session::Europe},
    {"EURAUD", Session::Asia},
    {"EURCHF", Session::Europe},
    {"EURGBP", Session::Europe},
    {"EURJPY", Session::Asia},
    {"EURNOK", Session::Europe},
    {"EURUSD", Session::Europe},
    {"FRAIDXEUR", Session::Europe},
    {"GBPJPY", Session::Asia},
    {"GBPUSD", Session::Europe},
    {"GBRIDXGBP", Session::Europe},
    {"HKGIDXHKD", Session::Asia},
    {"JPNIDXJPY", Session::Asia},
    {"LIGHTCMDUSD", Session::NewYork},
    {"NZDUSD", Session::Asia},
    {"USA30IDXUSD", Session::NewYork},
    {"USA500IDXUSD", Session::NewYork},
    {"USATECHIDXUSD", Session::NewYork},
    {"USDCAD", Session::NewYork},
    {"USDCHF", Session::Europe},
    {"USDJPY", Session::Asia},
    {"USDSEK", Session::Europe},
    {"XAGUSD", Session::NewYork},
    {"XAUUSD", Session::NewYork},
}};

static_assert(
    [] {
        for (std::size_t i = 1; i < kTable.size(); ++i) {
            if (!(kTable[i - 1].symbol < kTable[i].symbol)) {
                return false;
            }
        }
        return true;
    }(),
    "market_hours::kTable must stay sorted ascending by symbol — "
    "sessionFor binary-searches it");

// Binary search over kTable (same shape as symbol_scale::findEntry, with the
// overflow-safe midpoint). Unknown when the symbol is absent.
[[nodiscard]] constexpr Session sessionFor(std::string_view symbol) noexcept {
    std::size_t lo = 0;
    std::size_t hi = kTable.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        const Entry& entry = kTable[mid];
        if (entry.symbol < symbol) {
            lo = mid + 1;
        } else if (symbol < entry.symbol) {
            hi = mid;
        } else {
            return entry.session;
        }
    }
    return Session::Unknown;
}

// London summer time (BST): [last Sunday of March, last Sunday of October).
// Exported so tests can pin the transition dates directly.
[[nodiscard]] constexpr bool isLondonSummer(
    const std::chrono::sys_days day) noexcept {
    using namespace std::chrono;
    const year y = year_month_day{day}.year();
    const sys_days start{year_month_weekday_last{y, March, Sunday[last]}};
    const sys_days end{year_month_weekday_last{y, October, Sunday[last]}};
    return day >= start && day < end;
}

// New York summer time (EDT): [second Sunday of March, first Sunday of
// November).
[[nodiscard]] constexpr bool isNewYorkSummer(
    const std::chrono::sys_days day) noexcept {
    using namespace std::chrono;
    const year y = year_month_day{day}.year();
    const sys_days start{year_month_weekday{y, March, Sunday[2]}};
    const sys_days end{year_month_weekday{y, November, Sunday[1]}};
    return day >= start && day < end;
}

// True when a NEW ENTRY is allowed for `symbol` at `utcTimestamp` (see the
// header comment for the rules). Callers gate decide()/entries on this and
// nothing else.
[[nodiscard]] constexpr bool tradePermitted(
    const std::string_view symbol,
    const std::chrono::system_clock::time_point utcTimestamp) noexcept {
    using namespace std::chrono;
    const sys_days day = floor<days>(utcTimestamp);
    const weekday dow{day};
    // Time of day as a raw duration in [0h, 24h): compared against hours/
    // minutes constants via common_type, so the 13:30 US open needs no
    // whole-hour rounding and the window boundaries stay exact.
    const auto sinceMidnight = utcTimestamp - day;

    if (dow == Sunday) {
        return false;
    }
    if (dow == Friday && sinceMidnight >= hours{16}) {
        return false;
    }
    if (dow == Monday && sinceMidnight < hours{2}) {
        return false;
    }

    switch (sessionFor(symbol)) {
        case Session::Asia:
            return sinceMidnight < hours{6};
        case Session::Europe: {
            const minutes open{isLondonSummer(day) ? hours{7} : hours{8}};
            return sinceMidnight >= open && sinceMidnight < open + hours{3};
        }
        case Session::NewYork: {
            const minutes open =
                minutes{isNewYorkSummer(day) ? hours{13} : hours{14}} +
                minutes{30};
            return sinceMidnight >= open && sinceMidnight < open + hours{3};
        }
        case Session::Unknown:
            return false;
    }
    return false;  // unreachable, but keeps -Wreturn-type quiet
}

}  // namespace market_hours
