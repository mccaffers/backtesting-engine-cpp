// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

import databaseConnection;
import sqlManager;

namespace {

const std::chrono::system_clock::time_point kLower{
    std::chrono::sys_days{std::chrono::year{2025} / 10 / 1}};
const std::chrono::system_clock::time_point kUpper{
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 1}};

}  // namespace

TEST_CASE("buildMonthBoundariesQuery asks QuestDB for every boundary in one statement", "[sqlManager]") {
    // One statement means one now() evaluation, so all boundaries share a
    // single snapshot instant — the property the superset slicing relies on.
    CHECK(SqlManager::buildMonthBoundariesQuery(2) ==
          "SELECT now(), dateadd('M', -1, now()), dateadd('M', -2, now())");
    CHECK_THROWS_AS(SqlManager::buildMonthBoundariesQuery(0), std::invalid_argument);
}

TEST_CASE("formatTimestamp round-trips exactly through fastParseTimestamp", "[sqlManager]") {
    const auto tp = kLower + std::chrono::hours{14} + std::chrono::minutes{3} +
                    std::chrono::seconds{12} + std::chrono::microseconds{123456};
    const std::string formatted = SqlManager::formatTimestamp(tp);
    CHECK(formatted == "2025-10-01T14:03:12.123456Z");

    DateCache cache;
    CHECK(fastParseTimestamp(formatted.c_str(), cache) == tp);

    // Zero fractions still print all six digits and still round-trip.
    const std::string midnight = SqlManager::formatTimestamp(kLower);
    CHECK(midnight == "2025-10-01T00:00:00.000000Z");
    CHECK(fastParseTimestamp(midnight.c_str(), cache) == kLower);
}

TEST_CASE("buildPriceDataBetweenQuery uses literal bounds with half-open semantics", "[sqlManager]") {
    CHECK(SqlManager::buildPriceDataBetweenQuery({"EURUSD"}, kLower, kUpper) ==
          "SELECT 'EURUSD' as symbol, ask, bid, timestamp FROM 'EURUSD' "
          "WHERE timestamp >= cast('2025-10-01T00:00:00.000000Z' as timestamp) "
          "AND timestamp < cast('2026-01-01T00:00:00.000000Z' as timestamp) "
          "ORDER BY timestamp");
}

TEST_CASE("multi-symbol tick queries union and get a deterministic tie-break", "[sqlManager]") {
    const std::string query =
        SqlManager::buildPriceDataBetweenQuery({"EURUSD", "USDJPY"}, kLower, kUpper);
    CHECK(query.find("UNION ALL SELECT 'USDJPY'") != std::string::npos);
    // Equal-timestamp ticks across symbols must come back in one defined
    // order; single-symbol queries skip the tie-break so QuestDB can elide
    // the sort on the designated timestamp column.
    CHECK(query.ends_with(" ORDER BY timestamp, symbol"));
    CHECK(SqlManager::buildPriceDataBetweenQuery({"EURUSD"}, kLower, kUpper)
              .ends_with(" ORDER BY timestamp"));
}

TEST_CASE("loadPriceDataBetween rejects unknown symbols before touching the network", "[sqlManager]") {
    const DatabaseConnection db("test");
    CHECK_THROWS_AS(
        SqlManager::loadPriceDataBetween(db, {"EURUSD'; DROP TABLE x;--"}, kLower, kUpper),
        std::invalid_argument);
}
