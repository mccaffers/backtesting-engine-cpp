// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

import connectionFactory;
import databaseConnection;

TEST_CASE("DatabaseConnection builds the QuestDB connection string", "[db]") {
    DatabaseConnection db("test");
    std::string endpoint = "test";
    std::string connection_string =
        "host=" + endpoint + " "
        "port=8812 "
        "dbname=qdb "
        "user=admin "
        "password= "
        "connect_timeout=3";
    CHECK(connection_string == db.getConnectionString());
}

// Postgres wire-format text trims trailing zeros in the fractional seconds, so
// the parser must scale by the digits actually present: ".5" is 500000 µs. The
// old "%d" parse read it as 5 µs — a 100000x error that corrupted tick ordering
// at bar boundaries whenever QuestDB trimmed a timestamp.
TEST_CASE("fastParseTimestamp scales fractional seconds by digit count", "[db]") {
    DateCache cache;
    const auto base = std::chrono::system_clock::time_point{
        std::chrono::sys_days{std::chrono::year{2026} / 1 / 5}} + std::chrono::hours{9};
    const auto us = [](std::int64_t n) { return std::chrono::microseconds{n}; };

    CHECK(fastParseTimestamp("2026-01-05 09:00:00", cache) == base);
    CHECK(fastParseTimestamp("2026-01-05 09:00:00.5", cache) == base + us(500000));
    CHECK(fastParseTimestamp("2026-01-05 09:00:00.50", cache) == base + us(500000));
    CHECK(fastParseTimestamp("2026-01-05 09:00:00.500000", cache) == base + us(500000));
    CHECK(fastParseTimestamp("2026-01-05 09:00:00.000123", cache) == base + us(123));
    // Sub-microsecond digits are truncated, not misread.
    CHECK(fastParseTimestamp("2026-01-05 09:00:00.123456789", cache) == base + us(123456));

    CHECK_THROWS_AS(fastParseTimestamp("garbage", cache), InvalidTimestampFormatError);
    CHECK_THROWS_AS(fastParseTimestamp("2026-01-05 09:00:00.", cache),
                    InvalidTimestampFormatError);
}

TEST_CASE("connectionFromEnv resolves QuestDB settings from the environment", "[db]") {
    setenv("QUESTDB_HOST", "envhost", 1);
    setenv("QUESTDB_PORT", "9009", 1);

    CHECK(questdb::connectionFromEnv().getConnectionString() ==
          "host=envhost port=9009 dbname=qdb user=admin password=quest connect_timeout=3");

    // An explicit host (the run command passes its argv host) wins over
    // $QUESTDB_HOST; the port still comes from the environment.
    CHECK(questdb::connectionFromEnv("argvhost").getConnectionString() ==
          "host=argvhost port=9009 dbname=qdb user=admin password=quest connect_timeout=3");

    // Unset variables fall back to the local QuestDB defaults.
    unsetenv("QUESTDB_HOST");
    unsetenv("QUESTDB_PORT");
    CHECK(questdb::connectionFromEnv().getConnectionString() ==
          "host=127.0.0.1 port=8812 dbname=qdb user=admin password=quest connect_timeout=3");

    // A malformed port is rejected loudly, including trailing junk that
    // std::stoi would have silently truncated.
    setenv("QUESTDB_PORT", "8812x", 1);
    CHECK_THROWS_AS(questdb::connectionFromEnv(), std::runtime_error);
    unsetenv("QUESTDB_PORT");
}
