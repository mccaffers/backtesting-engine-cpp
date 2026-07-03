#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

import ohlcBuilder;
import ohlcObject;
import priceData;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

// Fixed simulation start; only the offsets between ticks matter to the builder.
const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// The builder reads only the timestamp from the tick — the price is passed
// separately (caller picks ask/bid/mid), so ask/bid here are placeholders.
PriceData tickAt(std::chrono::system_clock::duration offset) {
    return PriceData(110002, 110001, t0 + offset, "EURUSD");
}

}  // namespace

TEST_CASE("OhlcObject defaults mirror the C# sentinels", "[ohlc]") {
    const OhlcObject bar{};
    CHECK(bar.date == std::chrono::system_clock::time_point{});
    CHECK(bar.open == 0);
    CHECK(bar.close == 0);
    CHECK(bar.high == std::numeric_limits<std::int32_t>::min());
    CHECK(bar.low == std::numeric_limits<std::int32_t>::max());
    CHECK_FALSE(bar.complete);
}

TEST_CASE("first tick seeds a single in-progress bar", "[ohlc]") {
    std::vector<OhlcObject> bars;

    ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{5}, bars);

    REQUIRE(bars.size() == 1);
    CHECK(bars[0].date == t0);
    CHECK(bars[0].open == 110001);
    CHECK(bars[0].high == 110001);
    CHECK(bars[0].low == 110001);
    CHECK(bars[0].close == 110001);
    CHECK_FALSE(bars[0].complete);
}

TEST_CASE("ticks within the duration update the open bar in place", "[ohlc]") {
    std::vector<OhlcObject> bars;
    ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{5}, bars);

    SECTION("higher price raises high and moves close") {
        ohlc::calculateOHLC(tickAt(minutes{1}), 110010, minutes{5}, bars);

        REQUIRE(bars.size() == 1);
        CHECK(bars[0].open == 110001);
        CHECK(bars[0].high == 110010);
        CHECK(bars[0].low == 110001);
        CHECK(bars[0].close == 110010);
        CHECK_FALSE(bars[0].complete);
    }

    SECTION("lower price drops low and moves close") {
        ohlc::calculateOHLC(tickAt(minutes{2}), 109990, minutes{5}, bars);

        REQUIRE(bars.size() == 1);
        CHECK(bars[0].high == 110001);
        CHECK(bars[0].low == 109990);
        CHECK(bars[0].close == 109990);
    }

    SECTION("open and date never move after the seed tick") {
        ohlc::calculateOHLC(tickAt(minutes{1}), 110020, minutes{5}, bars);
        ohlc::calculateOHLC(tickAt(minutes{3}), 109980, minutes{5}, bars);

        REQUIRE(bars.size() == 1);
        CHECK(bars[0].date == t0);
        CHECK(bars[0].open == 110001);
        CHECK(bars[0].high == 110020);
        CHECK(bars[0].low == 109980);
        CHECK(bars[0].close == 109980);
    }
}

TEST_CASE("a tick exactly on the boundary stays in the open bar", "[ohlc]") {
    std::vector<OhlcObject> bars;
    ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{5}, bars);

    // Rollover requires strictly greater than the duration, matching the C#.
    ohlc::calculateOHLC(tickAt(minutes{5}), 110005, minutes{5}, bars);

    REQUIRE(bars.size() == 1);
    CHECK(bars[0].close == 110005);
    CHECK_FALSE(bars[0].complete);
}

TEST_CASE("a tick past the duration completes the bar and opens a new one", "[ohlc]") {
    std::vector<OhlcObject> bars;
    ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{5}, bars);
    ohlc::calculateOHLC(tickAt(minutes{2}), 110010, minutes{5}, bars);

    ohlc::calculateOHLC(tickAt(minutes{5} + seconds{1}), 110020, minutes{5}, bars);

    REQUIRE(bars.size() == 2);

    // Completed bar keeps its extremes; its close is overwritten with the new
    // bucket's first price (ported C# behaviour, bars join up).
    CHECK(bars[0].complete);
    CHECK(bars[0].open == 110001);
    CHECK(bars[0].high == 110010);
    CHECK(bars[0].low == 110001);
    CHECK(bars[0].close == 110020);

    // New bar is seeded entirely from the rollover tick.
    CHECK_FALSE(bars[1].complete);
    CHECK(bars[1].date == t0 + minutes{5} + seconds{1});
    CHECK(bars[1].open == 110020);
    CHECK(bars[1].high == 110020);
    CHECK(bars[1].low == 110020);
    CHECK(bars[1].close == 110020);
}

TEST_CASE("a stream spanning several buckets yields one bar per bucket", "[ohlc]") {
    std::vector<OhlcObject> bars;

    ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{5}, bars);
    ohlc::calculateOHLC(tickAt(minutes{6}), 110010, minutes{5}, bars);
    ohlc::calculateOHLC(tickAt(minutes{12}), 110020, minutes{5}, bars);
    ohlc::calculateOHLC(tickAt(minutes{13}), 110015, minutes{5}, bars);

    REQUIRE(bars.size() == 3);
    CHECK(bars[0].complete);
    CHECK(bars[1].complete);
    CHECK_FALSE(bars[2].complete);

    CHECK(bars[0].date == t0);
    CHECK(bars[1].date == t0 + minutes{6});
    CHECK(bars[2].date == t0 + minutes{12});

    CHECK(bars[2].open == 110020);
    CHECK(bars[2].high == 110020);
    CHECK(bars[2].low == 110015);
    CHECK(bars[2].close == 110015);
}
