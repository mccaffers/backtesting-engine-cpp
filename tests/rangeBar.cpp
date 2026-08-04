#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the builder off QuestDB
#include <stdexcept>
#include <string>
#include <vector>

import rangeBarBuilder;
import ohlcObject;
import priceData;

namespace {

using std::chrono::hours;
using std::chrono::microseconds;
using std::chrono::minutes;
using std::chrono::seconds;

// Fixed simulation start; the construct is deliberately clock-free, so only
// the ORDER of ticks matters — offsets exist to give bars distinct dates.
const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// Range bars build from the ASK; the bid rides 2 points under and is ignored.
PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 const std::string& symbol = "EURUSD") {
    return PriceData(ask, ask - 2, t0 + offset, symbol);
}

// Hermetic: a RangeSeries' first update fires the QuestDB warm-up when the
// gate is on (it defaults ON), so force it off for the whole binary before
// main — same pattern as tests/ohlc.cpp. The gate tests below flip it
// explicitly and restore "0" when done.
[[maybe_unused]] const bool kPrepopulateForcedOff = [] {
    setenv("OHLC_PREPOPULATE", "0", 1);
    return true;
}();

// The workhorse spec: a 4-tick window at 100% makes windowRange, thresholds
// and warm-up boundaries hand-computable.
constexpr rangebar::RangeBarSpec kSpec{.atrTickWindow = 4,
                                       .atrPercent = 100,
                                       .count = 8};

}  // namespace

TEST_CASE("RangeSeries rejects non-positive spec fields", "[rangeBar]") {
    using rangebar::RangeSeries;
    CHECK_THROWS_AS(RangeSeries({0, 100, 8}), std::invalid_argument);
    CHECK_THROWS_AS(RangeSeries({-1, 100, 8}), std::invalid_argument);
    CHECK_THROWS_AS(RangeSeries({4, 0, 8}), std::invalid_argument);
    CHECK_THROWS_AS(RangeSeries({4, -100, 8}), std::invalid_argument);
    CHECK_THROWS_AS(RangeSeries({4, 100, 0}), std::invalid_argument);
    CHECK_THROWS_AS(RangeSeries({4, 100, -8}), std::invalid_argument);
}

TEST_CASE("no bars form until the tick window is warm", "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);

    // Three ticks into a 4-tick window: measuring, not yet trading.
    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100010));
    s.update(tickAt(seconds{2}, 100005));
    CHECK_FALSE(s.warm());
    CHECK(s.bars().empty());

    // The 4th tick fills the window; bar #1 opens on this very tick.
    s.update(tickAt(seconds{3}, 100008));
    CHECK(s.warm());
    REQUIRE(s.bars().size() == 1);
    CHECK_FALSE(s.bars().front().complete);
}

TEST_CASE("rolling max and min slide and expire after exactly N ticks",
          "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);

    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100200));  // the spike, tick #2
    s.update(tickAt(seconds{2}, 100010));
    s.update(tickAt(seconds{3}, 100020));
    CHECK(s.windowRange() == 200);  // spike high vs tick-1 low

    // Tick #5 expires tick #1: the low rises to 100010, spike still in.
    s.update(tickAt(seconds{4}, 100030));
    CHECK(s.windowRange() == 190);

    // Tick #6 = spike + window: the spike stops influencing the measure on
    // exactly this tick — no clock involved, purely a count of events.
    s.update(tickAt(seconds{5}, 100040));
    CHECK(s.windowRange() == 30);
}

TEST_CASE("first bar opens on the first warm tick with its threshold locked",
          "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);
    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100002));
    s.update(tickAt(seconds{2}, 100001));
    s.update(tickAt(seconds{3}, 100010));  // warm; this tick IS the window max

    REQUIRE(s.bars().size() == 1);
    const OhlcObject& bar = s.bars().front();
    CHECK(bar.date == t0 + seconds{3});
    CHECK(bar.open == 100010);
    CHECK(bar.high == 100010);
    CHECK(bar.low == 100010);
    CHECK(bar.close == 100010);
    CHECK_FALSE(bar.complete);
    // The opening tick's own price counts: windowRange includes 100010, so
    // the locked threshold is 10, not the pre-tick 2.
    CHECK(s.lockedThreshold() == 10);
}

TEST_CASE("intra-threshold ticks fold into the in-progress bar", "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);
    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100002));
    s.update(tickAt(seconds{2}, 100001));
    s.update(tickAt(seconds{3}, 100010));  // bar opens, threshold 10

    s.update(tickAt(seconds{4}, 100015));
    s.update(tickAt(seconds{5}, 100008));

    REQUIRE(s.bars().size() == 1);
    const OhlcObject& bar = s.bars().front();
    CHECK(bar.open == 100010);
    CHECK(bar.high == 100015);
    CHECK(bar.low == 100008);
    CHECK(bar.close == 100008);
    CHECK_FALSE(bar.complete);  // high - low = 7 < 10
}

TEST_CASE("the breaching tick closes the bar and the next tick opens a new one",
          "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);
    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100002));
    s.update(tickAt(seconds{2}, 100001));
    s.update(tickAt(seconds{3}, 100010));  // bar opens, threshold 10
    s.update(tickAt(seconds{4}, 100014));

    // high - low hits the threshold ON this tick: included and closed at its
    // real price, immediately — not lazily when the successor opens.
    s.update(tickAt(seconds{5}, 100020));
    REQUIRE(s.bars().size() == 1);
    CHECK(s.bars().front().complete);
    CHECK(s.bars().front().high == 100020);
    CHECK(s.bars().front().low == 100010);
    CHECK(s.bars().front().close == 100020);

    // The successor is seeded entirely from the NEXT tick's real price.
    s.update(tickAt(seconds{6}, 100018));
    REQUIRE(s.bars().size() == 2);
    CHECK(s.bars()[0].complete);
    CHECK_FALSE(s.bars()[1].complete);
    CHECK(s.bars()[1].date == t0 + seconds{6});
    CHECK(s.bars()[1].open == 100018);
    CHECK(s.bars()[1].high == 100018);
    CHECK(s.bars()[1].low == 100018);
    CHECK(s.bars()[1].close == 100018);
}

TEST_CASE("a gap tick crossing many thresholds closes exactly one bar",
          "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);
    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100001));
    s.update(tickAt(seconds{2}, 100002));
    s.update(tickAt(seconds{3}, 100003));  // bar opens, threshold 3

    // ~33x the threshold in one tick: ONE bar completes, keeping the whole
    // overshoot as its real range — no phantom bars fill the gap.
    s.update(tickAt(seconds{4}, 100103));
    REQUIRE(s.bars().size() == 1);
    CHECK(s.bars().front().complete);
    CHECK(s.bars().front().close == 100103);
    CHECK(s.bars().front().high - s.bars().front().low == 100);

    // And the gap has already widened the measure: the successor locks a
    // threshold reflecting it (window {100002,100003,100103,100104} = 102).
    s.update(tickAt(seconds{5}, 100104));
    REQUIRE(s.bars().size() == 2);
    CHECK(s.lockedThreshold() == 102);
}

TEST_CASE("the locked threshold never moves mid-bar; the next bar adapts "
          "instantly", "[rangeBar]") {
    rangebar::RangeSeries s(kSpec);
    s.update(tickAt(seconds{0}, 100000));
    s.update(tickAt(seconds{1}, 100001));
    s.update(tickAt(seconds{2}, 100002));
    s.update(tickAt(seconds{3}, 100003));  // bar 1 opens, threshold 3
    CHECK(s.lockedThreshold() == 3);

    // The measure drifts to 4 (window {100001..100005}) but the open bar's
    // target stays the locked 3.
    s.update(tickAt(seconds{4}, 100005));
    CHECK(s.windowRange() == 4);
    CHECK(s.lockedThreshold() == 3);
    CHECK_FALSE(s.bars().front().complete);

    // The bar closes on its ORIGINAL threshold (range 3), even though the
    // floating measure says 4 — that is what lock-at-open means.
    s.update(tickAt(seconds{5}, 100006));
    REQUIRE(s.bars().size() == 1);
    CHECK(s.bars().front().complete);
    CHECK(s.bars().front().high - s.bars().front().low == 3);

    // A spike on the very next tick: the successor opens with a threshold
    // that already reflects it. No time bucket to wait out — the design's
    // whole point.
    s.update(tickAt(seconds{6}, 100100));
    REQUIRE(s.bars().size() == 2);
    CHECK(s.lockedThreshold() == 97);  // window {100003,100005,100006,100100}
}

TEST_CASE("the threshold floors at one point", "[rangeBar]") {
    // 1% of a dead-flat window rounds to 0; the floor keeps it at 1 point.
    rangebar::RangeSeries s({.atrTickWindow = 4, .atrPercent = 1, .count = 8});
    for (int i = 0; i < 4; ++i) {
        s.update(tickAt(seconds{i}, 100000));
    }
    REQUIRE(s.bars().size() == 1);
    CHECK(s.lockedThreshold() == 1);

    SECTION("a dead-flat stream holds one open bar, never a bar per tick") {
        for (int i = 4; i < 20; ++i) {
            s.update(tickAt(seconds{i}, 100000));
        }
        REQUIRE(s.bars().size() == 1);
        CHECK_FALSE(s.bars().front().complete);
    }

    SECTION("a one-point move meets the floored threshold") {
        s.update(tickAt(seconds{4}, 100001));
        REQUIRE(s.bars().size() == 1);
        CHECK(s.bars().front().complete);
    }
}

TEST_CASE("identical price sequences build identical bars regardless of tick "
          "spacing", "[rangeBar]") {
    const std::vector<std::int32_t> prices = {100000, 100050, 100010, 100020,
                                              100030, 100060, 100005, 100040};

    // Same prices, wildly different clocks: microseconds apart vs hours
    // apart (spanning days). The clock never enters the construct.
    rangebar::RangeSeries fast(kSpec);
    rangebar::RangeSeries slow(kSpec);
    for (std::size_t i = 0; i < prices.size(); ++i) {
        fast.update(tickAt(microseconds{i}, prices[i]));
        slow.update(tickAt(hours{7 * i}, prices[i]));
    }

    REQUIRE(fast.bars().size() == slow.bars().size());
    REQUIRE_FALSE(fast.bars().empty());
    for (std::size_t i = 0; i < fast.bars().size(); ++i) {
        CHECK(fast.bars()[i].open == slow.bars()[i].open);
        CHECK(fast.bars()[i].high == slow.bars()[i].high);
        CHECK(fast.bars()[i].low == slow.bars()[i].low);
        CHECK(fast.bars()[i].close == slow.bars()[i].close);
        CHECK(fast.bars()[i].complete == slow.bars()[i].complete);
    }
    CHECK(fast.lockedThreshold() == slow.lockedThreshold());
}

TEST_CASE("prepopulateTicksQuery pins the QuestDB SQL shape", "[rangeBar]") {
    // Bounds are hardcoded, not recomputed with the same arithmetic: t0 is
    // 2026-01-05T09:00Z = 1767603600000000us, and the cap for {4, 100, 8} is
    // 4 (exact window warm-up) + 2*8*4 (bar-formation margin) = 68.
    CHECK(rangebar::prepopulateTicksQuery("EURUSD", t0, kSpec) ==
          "SELECT 'EURUSD' as symbol, ask, bid, timestamp FROM 'EURUSD' "
          "WHERE timestamp < cast(1767603600000000L AS timestamp) "
          "ORDER BY timestamp DESC LIMIT 68");
}

TEST_CASE("prepopulation gate off falls back to a cold start", "[rangeBar]") {
    // Explicit "0", not unsetenv: the gate defaults ON, so unset means on.
    setenv("OHLC_PREPOPULATE", "0", 1);

    // A 1-tick window is warm from the very first tick, so the cold start is
    // visible immediately: one bar seeded from the live tick, no DB rows.
    rangebar::RangeSeries s({.atrTickWindow = 1, .atrPercent = 100, .count = 4});
    s.update(tickAt(seconds{0}, 100000));

    REQUIRE(s.bars().size() == 1);
    CHECK(s.bars().front().date == t0);
    CHECK(s.bars().front().open == 100000);
    CHECK_FALSE(s.bars().front().complete);
}

TEST_CASE("unknown symbol yields empty without touching the DB", "[rangeBar]") {
    // The symbol guard precedes any connection, so this passes gate-on with
    // no QuestDB running (and doubles as the SQL-injection whitelist check).
    setenv("OHLC_PREPOPULATE", "1", 1);
    const auto ticks = rangebar::prepopulateTicks("NOPE", t0, kSpec);
    setenv("OHLC_PREPOPULATE", "0", 1);  // back to the binary's hermetic state

    CHECK(ticks.empty());
}

// Hidden ([.]) — needs a live QuestDB with EURUSD ticks. Run explicitly:
//   ./build/tests/unit_tests "[dblive]"
TEST_CASE("prepopulateTicks fetches ascending ticks from a live QuestDB",
          "[.][dblive]") {
    const rangebar::RangeBarSpec spec{.atrTickWindow = 1000,
                                      .atrPercent = 100,
                                      .count = 24};
    setenv("OHLC_PREPOPULATE", "1", 1);
    const auto ticks = rangebar::prepopulateTicks(
        "EURUSD", std::chrono::system_clock::now(), spec);
    setenv("OHLC_PREPOPULATE", "0", 1);  // back to the binary's hermetic state

    REQUIRE_FALSE(ticks.empty());
    CHECK(std::cmp_less_equal(ticks.size(), rangebar::prepopulateTickCap(spec)));
    CHECK(std::ranges::is_sorted(ticks, {}, &PriceData::timestamp));
    for (const PriceData& tick : ticks) {
        CHECK(tick.ask > 0);  // scaled INT prices parsed, not garbage
        CHECK(tick.bid > 0);
        CHECK(tick.symbol == "EURUSD");
    }
}
