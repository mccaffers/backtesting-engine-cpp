#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv/unsetenv (POSIX)
#include <limits>
#include <stdexcept>
#include <string>
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

// The engine's prepopulate gate is ON by default (see ohlcBuilder), but unit
// tests must stay hermetic: any test that hands calculateOHLC an empty bar
// list and a prepopulate count would otherwise fire real QuestDB queries
// (the breakout-strategy and live-runner tests all do). Force the gate off
// for the whole binary before main; the gate tests below flip it explicitly
// and restore "0" when done.
[[maybe_unused]] const bool kPrepopulateForcedOff = [] {
    setenv("OHLC_PREPOPULATE", "0", 1);
    return true;
}();

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

    // Completed bar keeps its extremes AND its own bucket's last price as the
    // close — the rollover tick belongs entirely to the new bar. (The old
    // C#-ported close-join overwrote it with the next bucket's first price,
    // which could push close outside [low, high] on a gap and disagreed with
    // the prepopulate query's last(ask) convention.)
    CHECK(bars[0].complete);
    CHECK(bars[0].open == 110001);
    CHECK(bars[0].high == 110010);
    CHECK(bars[0].low == 110001);
    CHECK(bars[0].close == 110010);

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

// A zero/negative duration would roll a new bar on every tick — one bar per
// tick over months of ticks is an OOM, so the builder rejects it up front.
TEST_CASE("non-positive bar duration throws instead of exploding", "[ohlc]") {
    std::vector<OhlcObject> bars;
    CHECK_THROWS_AS(
        ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{0}, bars),
        std::invalid_argument);
    CHECK_THROWS_AS(
        ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{-5}, bars),
        std::invalid_argument);
    CHECK(bars.empty());
}

TEST_CASE("prepopulateQuery pins the QuestDB SQL shape", "[ohlc]") {
    // Bounds are hardcoded, not recomputed with the same chrono ops: t0 is
    // 2026-01-05T09:00Z = 1767603600000000us, and 5m x 24 bars -> a 16-day
    // lookback (ceil(120/1440)*6 + 10) = 1382400000000us earlier.
    CHECK(ohlc::prepopulateQuery("EURUSD", t0, minutes{5}, 24) ==
          "SELECT timestamp, open, high, low, close FROM ("
          "SELECT timestamp, first(ask) AS open, max(ask) AS high, min(ask) AS low, "
          "last(ask) AS close, count() AS ticks FROM 'EURUSD' "
          "WHERE timestamp >= cast(1766221200000000L AS timestamp) "
          "AND timestamp < cast(1767603600000000L AS timestamp) "
          "SAMPLE BY 5m ALIGN TO CALENDAR"
          ") WHERE ticks >= 10 ORDER BY timestamp DESC LIMIT 24");
}

TEST_CASE("prepopulation gate off falls back to the cold tick seed", "[ohlc]") {
    // Explicit "0", not unsetenv: the gate defaults ON, so unset means on.
    setenv("OHLC_PREPOPULATE", "0", 1);
    std::vector<OhlcObject> bars;

    ohlc::calculateOHLC(tickAt(minutes{0}), 110001, minutes{5}, bars, 3);

    REQUIRE(bars.size() == 1);
    CHECK(bars[0].date == t0);
    CHECK(bars[0].open == 110001);
    CHECK(bars[0].close == 110001);
    CHECK_FALSE(bars[0].complete);
}

TEST_CASE("unknown symbol yields empty without touching the DB", "[ohlc]") {
    // The symbol guard precedes any connection, so this passes gate-on with no
    // QuestDB running (and doubles as the SQL-injection whitelist check).
    setenv("OHLC_PREPOPULATE", "1", 1);
    const auto bars = ohlc::prepopulateOHLC("NOPE", t0, minutes{5}, 24);
    setenv("OHLC_PREPOPULATE", "0", 1);  // back to the binary's hermetic state

    CHECK(bars.empty());
}

// Hidden ([.]) — needs a live QuestDB with EURUSD ticks. Run explicitly:
//   ./build/tests/unit_tests "[dblive]"
TEST_CASE("prepopulateOHLC fetches ascending bars from a live QuestDB", "[.][dblive]") {
    setenv("OHLC_PREPOPULATE", "1", 1);
    const auto bars = ohlc::prepopulateOHLC("EURUSD", std::chrono::system_clock::now(),
                                            minutes{5}, 24);
    setenv("OHLC_PREPOPULATE", "0", 1);  // back to the binary's hermetic state

    REQUIRE_FALSE(bars.empty());
    CHECK(bars.size() <= 24);
    CHECK(std::ranges::is_sorted(bars, {}, &OhlcObject::date));
    for (std::size_t i = 0; i < bars.size(); ++i) {
        CHECK(bars[i].complete == (i + 1 < bars.size()));  // only back() in progress
        CHECK(bars[i].low <= bars[i].high);
        CHECK(bars[i].low > 0);  // scaled INT prices parsed, not garbage
    }
}
