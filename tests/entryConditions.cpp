#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the store off QuestDB
#include <string>

#include "shared/tradingDefinitions/strategyConfig.hpp"

import barStore;
import entryConditions;
import priceData;

namespace {

using std::chrono::minutes;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// The gate's series as production registers it: 15m bars, ATR(10)'s 11-bar
// window (conditions::gateSeriesFor's fallback shape).
const bars::SeriesSpec kGate{minutes{15}, 11};

// A store fed `ticks` zero-spread ticks 16 minutes apart, each rolling a
// fresh 15m bar and stepping the ask by `stepPoints` — so once 11 bars exist,
// every true range is `stepPoints` and ATR(10) reads exactly that.
bars::BarStore warmStore(int ticks, std::int32_t stepPoints,
                         const std::string& symbol = "EURUSD") {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query
    bars::BarStore store;
    store.registerSeries(kGate.minutes, kGate.count);
    for (int i = 0; i < ticks; ++i) {
        const std::int32_t price = 110000 + i * stepPoints;
        store.update(PriceData(price, price, t0 + minutes{16 * i}, symbol));
    }
    return store;
}

// The tick under judgement: only its bid/ask (the spread) and symbol matter
// to check() — the ATR comes from the store's bars.
PriceData tickWithSpread(std::int32_t spreadPoints,
                         const std::string& symbol = "EURUSD") {
    return PriceData(110000 + spreadPoints, 110000, t0 + std::chrono::hours{5},
                     symbol);
}

}  // namespace

TEST_CASE("conditions::check is nullopt until the gate series warms",
          "[entryConditions]") {
    // 10 bars < the 11 ATR(10) needs -> not warm; the 11th bar arms it.
    const auto cold = warmStore(10, 200);
    CHECK_FALSE(conditions::check(cold, kGate, tickWithSpread(0), 1, 3));

    const auto warm = warmStore(11, 200);
    const auto distances = conditions::check(warm, kGate, tickWithSpread(0), 1, 3);
    REQUIRE(distances.has_value());
    // ATR 200 points = 20 EURUSD pips: stop = 20 x 1, limit = 20 x 3.
    CHECK(distances->stopPips == 20);
    CHECK(distances->limitPips == 60);
}

TEST_CASE("conditions::check rejects a spread above 30% of ATR",
          "[entryConditions]") {
    // ATR 200 points -> the spread ceiling is exactly 60 points.
    const auto store = warmStore(12, 200);
    CHECK(conditions::check(store, kGate, tickWithSpread(60), 1, 3).has_value());
    CHECK_FALSE(conditions::check(store, kGate, tickWithSpread(61), 1, 3));
}

TEST_CASE("conditions::check enforces the volatility floors in points",
          "[entryConditions]") {
    SECTION("stop floor: ATR x mult below 10 pips skips the entry") {
        // ATR 99 points x 1 = 99 < 100 (10 pips x 10 points/pip) -> skip;
        // ATR 100 x 1 = 100 passes and lands exactly on the 10-pip floor.
        const auto below = warmStore(12, 99);
        CHECK_FALSE(conditions::check(below, kGate, tickWithSpread(0), 1, 9));

        const auto at = warmStore(12, 100);
        const auto distances = conditions::check(at, kGate, tickWithSpread(0), 1, 9);
        REQUIRE(distances.has_value());
        CHECK(distances->stopPips == 10);
        CHECK(distances->limitPips == 90);
    }

    SECTION("limit floor: 3 pips") {
        // ATR 25 points x 4 = 100 clears the stop floor, but x 1 = 25 < 30
        // (3 pips x 10) fails the limit floor.
        const auto store = warmStore(12, 25);
        CHECK_FALSE(conditions::check(store, kGate, tickWithSpread(0), 4, 1));
        const auto distances = conditions::check(store, kGate, tickWithSpread(0), 4, 2);
        REQUIRE(distances.has_value());
        CHECK(distances->stopPips == 10);  // 100 points
        CHECK(distances->limitPips == 5);  // 50 points
    }

    SECTION("a zero multiplier never trades") {
        const auto store = warmStore(12, 200);
        CHECK_FALSE(conditions::check(store, kGate, tickWithSpread(0), 0, 3));
        CHECK_FALSE(conditions::check(store, kGate, tickWithSpread(0), 1, 0));
    }
}

TEST_CASE("conditions::check clamps blown-out volatility", "[entryConditions]") {
    // ATR 2000 points = 200 pips: stop 400 pips -> 80, limit 1800 -> 300.
    const auto store = warmStore(12, 2000);
    const auto distances = conditions::check(store, kGate, tickWithSpread(0), 2, 9);
    REQUIRE(distances.has_value());
    CHECK(distances->stopPips == 80);
    CHECK(distances->limitPips == 300);
}

TEST_CASE("conditions::check refuses symbols outside symbol_scale",
          "[entryConditions]") {
    const auto store = warmStore(12, 200, "NOPEUSD");
    CHECK_FALSE(
        conditions::check(store, kGate, tickWithSpread(0, "NOPEUSD"), 1, 3));
}

TEST_CASE("conditions::gateSeriesFor picks the primary timeframe or falls back",
          "[entryConditions]") {
    tradingDefinitions::StrategyConfig config;

    SECTION("primary OHLC timeframe, window already deep enough") {
        config.OHLC_VARIABLES = {
            tradingDefinitions::OHLCVariables{.OHLC_COUNT = 24, .OHLC_MINUTES = 15},
            tradingDefinitions::OHLCVariables{.OHLC_COUNT = 50, .OHLC_MINUTES = 60},
        };
        const auto series = conditions::gateSeriesFor(config);
        CHECK(series.minutes == minutes{15});
        CHECK(series.count == 24);
    }

    SECTION("primary window shallower than ATR(10) is widened to 11") {
        config.OHLC_VARIABLES = {
            tradingDefinitions::OHLCVariables{.OHLC_COUNT = 5, .OHLC_MINUTES = 30},
        };
        const auto series = conditions::gateSeriesFor(config);
        CHECK(series.minutes == minutes{30});
        CHECK(series.count == 11);
    }

    SECTION("no OHLC timeframes: the 15m fallback") {
        const auto series = conditions::gateSeriesFor(config);
        CHECK(series.minutes == minutes{15});
        CHECK(series.count == 11);
    }

    SECTION("the {0,0} 'builds no bars' sentinel also falls back") {
        config.OHLC_VARIABLES = {tradingDefinitions::OHLCVariables{}};
        const auto series = conditions::gateSeriesFor(config);
        CHECK(series.minutes == minutes{15});
        CHECK(series.count == 11);
    }
}
