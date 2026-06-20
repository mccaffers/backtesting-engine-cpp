// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <string>

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
