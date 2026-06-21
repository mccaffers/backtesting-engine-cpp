// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <string>

import symbolScale;

// points-per-pip: FX majors & JPY pairs = 10, indices/commodities = 100,
// metals = 1000.
TEST_CASE("symbol_scale::get maps symbols to points-per-pip", "[symbolScale]") {
    SECTION("FX majors") {
        CHECK(symbol_scale::get("EURUSD") == 10);
        CHECK(symbol_scale::get("AUDUSD") == 10);
        CHECK(symbol_scale::get("GBPUSD") == 10);
        CHECK(symbol_scale::get("USDCAD") == 10);
        CHECK(symbol_scale::get("EURNOK") == 10);
    }

    SECTION("JPY pairs and metals") {
        CHECK(symbol_scale::get("USDJPY") == 10);
        CHECK(symbol_scale::get("GBPJPY") == 10);
        CHECK(symbol_scale::get("EURJPY") == 10);
        CHECK(symbol_scale::get("XAUUSD") == 1000);
        CHECK(symbol_scale::get("XAGUSD") == 1000);
    }

    SECTION("indices and commodities") {
        CHECK(symbol_scale::get("USA500IDXUSD") == 100);
        CHECK(symbol_scale::get("USATECHIDXUSD") == 100);
        CHECK(symbol_scale::get("AUSIDXAUD") == 100);
        CHECK(symbol_scale::get("BRENTCMDUSD") == 100);
    }

    SECTION("boundary entries") {
        CHECK(symbol_scale::get("AUDNZD") == 10);
        CHECK(symbol_scale::get("XAGUSD") == 1000);
    }
}

TEST_CASE("symbol_scale::get rejects unknown and malformed symbols", "[symbolScale]") {
    SECTION("unknown symbols return the sentinel") {
        CHECK(symbol_scale::get("NOPE") == symbol_scale::kUnknown);
        CHECK(symbol_scale::get("") == symbol_scale::kUnknown);
        CHECK(symbol_scale::get("EURUSDX") == symbol_scale::kUnknown);
    }

    SECTION("lookup is case sensitive") {
        CHECK(symbol_scale::get("eurusd") == symbol_scale::kUnknown);
    }

    SECTION("accepts std::string") {
        std::string s = "USDCHF";
        CHECK(symbol_scale::get(s) == 10);
    }
}
