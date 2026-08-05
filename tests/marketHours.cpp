// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// market_hours — the peak-hours entry filter. Session mapping, the hand-rolled
// DST rules (pinned against known 2026 transition dates), the weekend block,
// and every session window's edges in both summer and winter time.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

import marketHours;
import symbolScale;

namespace {

using namespace std::chrono;
using market_hours::Session;

// UTC instant on a calendar date at hh:mm.
constexpr system_clock::time_point at(const year_month_day date, const hours h,
                                      const minutes m = minutes{0}) {
    return sys_days{date} + h + m;
}

// 2026 calendar facts the window tests lean on: 2026-07-15 and 2026-01-14 are
// Wednesdays (mid-week, no weekend rule in play); BST runs [Sun 2026-03-29,
// Sun 2026-10-25) and EDT runs [Sun 2026-03-08, Sun 2026-11-01).
constexpr year_month_day kSummerWed = 2026y / 7 / 15;
constexpr year_month_day kWinterWed = 2026y / 1 / 14;

}  // namespace

TEST_CASE("sessionFor maps each symbol to its market session", "[marketHours]") {
    CHECK(market_hours::sessionFor("USDJPY") == Session::Asia);
    CHECK(market_hours::sessionFor("JPNIDXJPY") == Session::Asia);
    CHECK(market_hours::sessionFor("AUDNZD") == Session::Asia);
    CHECK(market_hours::sessionFor("EURUSD") == Session::Europe);
    CHECK(market_hours::sessionFor("GBRIDXGBP") == Session::Europe);
    CHECK(market_hours::sessionFor("USDSEK") == Session::Europe);
    CHECK(market_hours::sessionFor("USA500IDXUSD") == Session::NewYork);
    CHECK(market_hours::sessionFor("XAUUSD") == Session::NewYork);
    CHECK(market_hours::sessionFor("BRENTCMDUSD") == Session::NewYork);
    CHECK(market_hours::sessionFor("DOGEUSD") == Session::Unknown);
    CHECK(market_hours::sessionFor("") == Session::Unknown);
}

// The session table and the price table must cover the SAME symbol universe
// (same pin as the marketDefinitions cross-table test): a symbol priced but
// unmapped here would silently never trade under the filter, and a mapped
// symbol without a price scale could not be traded at all.
TEST_CASE("every priced symbol has a session and vice versa", "[marketHours]") {
    CHECK(market_hours::kTable.size() == symbol_scale::kTable.size());
    for (const auto& entry : symbol_scale::kTable) {
        INFO("symbol_scale entry missing a session: " << entry.symbol);
        CHECK(market_hours::sessionFor(entry.symbol) != Session::Unknown);
    }
    for (const auto& entry : market_hours::kTable) {
        INFO("market_hours entry missing a price scale: " << entry.symbol);
        CHECK(symbol_scale::get(entry.symbol) != symbol_scale::kUnknown);
    }
}

TEST_CASE("DST helpers pin the 2026 transition dates", "[marketHours]") {
    SECTION("London: [last Sunday of March, last Sunday of October)") {
        CHECK_FALSE(market_hours::isLondonSummer(sys_days{2026y / 3 / 28}));
        CHECK(market_hours::isLondonSummer(sys_days{2026y / 3 / 29}));
        CHECK(market_hours::isLondonSummer(sys_days{2026y / 10 / 24}));
        CHECK_FALSE(market_hours::isLondonSummer(sys_days{2026y / 10 / 25}));
    }

    SECTION("New York: [second Sunday of March, first Sunday of November)") {
        CHECK_FALSE(market_hours::isNewYorkSummer(sys_days{2026y / 3 / 7}));
        CHECK(market_hours::isNewYorkSummer(sys_days{2026y / 3 / 8}));
        CHECK(market_hours::isNewYorkSummer(sys_days{2026y / 10 / 31}));
        CHECK_FALSE(market_hours::isNewYorkSummer(sys_days{2026y / 11 / 1}));
    }
}

// Times below sit inside the symbol's session window, so only the weekend
// rule varies.
TEST_CASE("tradePermitted blocks the weekend and its shoulders", "[marketHours]") {
    SECTION("Friday cuts off at 16:00 UTC") {
        // US summer window 13:30-16:30 straddles the cutoff.
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 7 / 17, hours{15}, minutes{59})));
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 7 / 17, hours{16})));
    }

    SECTION("all of Sunday is blocked") {
        CHECK_FALSE(market_hours::tradePermitted(
            "USDJPY", at(2026y / 7 / 19, hours{3})));
    }

    SECTION("Monday opens at 02:00 UTC") {
        CHECK_FALSE(market_hours::tradePermitted(
            "USDJPY", at(2026y / 7 / 20, hours{1}, minutes{59})));
        CHECK(market_hours::tradePermitted(
            "USDJPY", at(2026y / 7 / 20, hours{2})));
    }
}

TEST_CASE("Asia window is 00:00-06:00 UTC in every season", "[marketHours]") {
    for (const year_month_day date : {kSummerWed, kWinterWed}) {
        INFO("date " << static_cast<int>(static_cast<unsigned>(date.month())));
        CHECK(market_hours::tradePermitted("USDJPY", at(date, hours{0})));
        CHECK(market_hours::tradePermitted("USDJPY",
                                           at(date, hours{5}, minutes{59})));
        CHECK_FALSE(market_hours::tradePermitted("USDJPY", at(date, hours{6})));
    }
}

TEST_CASE("Europe window tracks the London open across DST", "[marketHours]") {
    SECTION("summer (BST): 07:00-10:00 UTC") {
        CHECK_FALSE(market_hours::tradePermitted(
            "EURUSD", at(kSummerWed, hours{6}, minutes{59})));
        CHECK(market_hours::tradePermitted("EURUSD", at(kSummerWed, hours{7})));
        CHECK(market_hours::tradePermitted(
            "EURUSD", at(kSummerWed, hours{9}, minutes{59})));
        CHECK_FALSE(market_hours::tradePermitted("EURUSD",
                                                 at(kSummerWed, hours{10})));
    }

    SECTION("winter (GMT): 08:00-11:00 UTC") {
        CHECK_FALSE(market_hours::tradePermitted(
            "EURUSD", at(kWinterWed, hours{7}, minutes{59})));
        CHECK(market_hours::tradePermitted("EURUSD", at(kWinterWed, hours{8})));
        CHECK(market_hours::tradePermitted(
            "EURUSD", at(kWinterWed, hours{10}, minutes{59})));
        CHECK_FALSE(market_hours::tradePermitted("EURUSD",
                                                 at(kWinterWed, hours{11})));
    }
}

TEST_CASE("US window tracks the New York open across DST", "[marketHours]") {
    SECTION("summer (EDT): 13:30-16:30 UTC") {
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(kSummerWed, hours{13}, minutes{29})));
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(kSummerWed, hours{13}, minutes{30})));
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(kSummerWed, hours{16}, minutes{29})));
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(kSummerWed, hours{16}, minutes{30})));
    }

    SECTION("winter (EST): 14:30-17:30 UTC") {
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(kWinterWed, hours{14}, minutes{29})));
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(kWinterWed, hours{14}, minutes{30})));
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(kWinterWed, hours{17}, minutes{29})));
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(kWinterWed, hours{17}, minutes{30})));
    }
}

TEST_CASE("unknown symbols never trade under the filter", "[marketHours]") {
    // A time inside every session's window and clear of the weekend rules.
    CHECK_FALSE(market_hours::tradePermitted(
        "DOGEUSD", at(kSummerWed, hours{14}, minutes{45})));
}

// The Friday before and the Monday after each transition must use that day's
// own open; the transition Sunday itself is weekend-blocked, which is what
// makes date-level DST granularity exact.
TEST_CASE("DST boundary weeks switch opens Friday-to-Monday", "[marketHours]") {
    SECTION("London spring forward (Sun 2026-03-29)") {
        CHECK_FALSE(market_hours::tradePermitted(
            "EURUSD", at(2026y / 3 / 27, hours{7}, minutes{30})));  // GMT Friday
        CHECK(market_hours::tradePermitted(
            "EURUSD", at(2026y / 3 / 27, hours{8}, minutes{30})));
        CHECK_FALSE(market_hours::tradePermitted(
            "EURUSD", at(2026y / 3 / 29, hours{8}, minutes{30})));  // Sunday
        CHECK(market_hours::tradePermitted(
            "EURUSD", at(2026y / 3 / 30, hours{7}, minutes{30})));  // BST Monday
        CHECK_FALSE(market_hours::tradePermitted(
            "EURUSD", at(2026y / 3 / 30, hours{10}, minutes{30})));
    }

    SECTION("London fall back (Sun 2026-10-25)") {
        CHECK(market_hours::tradePermitted(
            "EURUSD", at(2026y / 10 / 23, hours{7}, minutes{30})));  // BST Friday
        CHECK_FALSE(market_hours::tradePermitted(
            "EURUSD", at(2026y / 10 / 26, hours{7}, minutes{30})));  // GMT Monday
        CHECK(market_hours::tradePermitted(
            "EURUSD", at(2026y / 10 / 26, hours{8}, minutes{30})));
    }

    SECTION("New York spring forward (Sun 2026-03-08)") {
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 3 / 6, hours{13}, minutes{45})));  // EST Fri
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 3 / 6, hours{14}, minutes{45})));
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 3 / 9, hours{13}, minutes{45})));  // EDT Mon
    }

    SECTION("New York fall back (Sun 2026-11-01)") {
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 10 / 30, hours{13}, minutes{45})));  // EDT Fri
        CHECK_FALSE(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 11 / 2, hours{13}, minutes{45})));  // EST Mon
        CHECK(market_hours::tradePermitted(
            "USA500IDXUSD", at(2026y / 11 / 2, hours{14}, minutes{45})));
    }
}
