#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the store off QuestDB
#include <stdexcept>
#include <string>

import barStore;
import ohlcObject;
import priceData;
import rangeBarBuilder;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// Bars build from the ASK; the bid rides 2 points under and is ignored here.
PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 const std::string& symbol = "EURUSD") {
    return PriceData(ask, ask - 2, t0 + offset, symbol);
}

bars::BarStore makeStore() {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query
    return bars::BarStore{};
}

}  // namespace

TEST_CASE("BarStore rejects unusable series", "[barStore]") {
    auto store = makeStore();
    CHECK_THROWS_AS(store.registerSeries(minutes{0}, 5), std::invalid_argument);
    CHECK_THROWS_AS(store.registerSeries(minutes{15}, 0), std::invalid_argument);
}

TEST_CASE("BarStore builds, rolls and trims a registered series", "[barStore]") {
    auto store = makeStore();
    store.registerSeries(minutes{15}, 3);

    // Two ticks inside the first 15m window aggregate into ONE bar.
    store.update(tickAt(minutes{0}, 110000));
    store.update(tickAt(minutes{10}, 110050));
    const auto* series = store.find("EURUSD", minutes{15});
    REQUIRE(series != nullptr);
    REQUIRE(series->size() == 1);
    CHECK(series->front().open == 110000);
    CHECK(series->front().high == 110050);
    CHECK(series->front().low == 110000);
    CHECK(series->front().close == 110050);
    CHECK_FALSE(series->front().complete);

    // 16 minutes on rolls a new bar; the finished one is marked complete.
    store.update(tickAt(minutes{16}, 110100));
    REQUIRE(series->size() == 2);
    CHECK(series->front().complete);
    CHECK(series->back().open == 110100);

    // Two more rolls exceed the window of 3: the oldest bar drops off.
    store.update(tickAt(minutes{32}, 110200));
    store.update(tickAt(minutes{48}, 110300));
    REQUIRE(series->size() == 3);
    CHECK(series->front().open == 110100);  // the 110000 bar was trimmed
    CHECK(series->back().open == 110300);
}

TEST_CASE("BarStore dedups a re-registered duration keeping the larger window",
          "[barStore]") {
    auto store = makeStore();
    store.registerSeries(minutes{15}, 3);
    store.registerSeries(minutes{15}, 5);  // same timeframe, deeper window

    for (int i = 0; i < 7; ++i) {
        store.update(tickAt(minutes{16 * i}, 110000 + i * 10));
    }
    const auto* series = store.find("EURUSD", minutes{15});
    REQUIRE(series != nullptr);
    CHECK(series->size() == 5);  // one shared history at the deeper window
}

TEST_CASE("BarStore keeps registered timeframes independent", "[barStore]") {
    auto store = makeStore();
    store.registerSeries(minutes{1}, 10);
    store.registerSeries(minutes{60}, 10);

    // 2-minute spacing rolls a 1m bar per tick but stays inside one 60m bar.
    for (int i = 0; i < 5; ++i) {
        store.update(tickAt(minutes{2 * i}, 110000 + i));
    }
    REQUIRE(store.find("EURUSD", minutes{1}) != nullptr);
    REQUIRE(store.find("EURUSD", minutes{60}) != nullptr);
    CHECK(store.find("EURUSD", minutes{1})->size() == 5);
    CHECK(store.find("EURUSD", minutes{60})->size() == 1);
}

TEST_CASE("BarStore keeps per-symbol histories isolated", "[barStore]") {
    auto store = makeStore();
    store.registerSeries(minutes{15}, 4);

    store.update(tickAt(minutes{0}, 110000, "EURUSD"));
    store.update(tickAt(seconds{30}, 65000, "AUDUSD"));
    store.update(tickAt(minutes{16}, 110100, "EURUSD"));

    const auto* eur = store.find("EURUSD", minutes{15});
    const auto* aud = store.find("AUDUSD", minutes{15});
    REQUIRE(eur != nullptr);
    REQUIRE(aud != nullptr);
    CHECK(eur->size() == 2);
    CHECK(aud->size() == 1);
    CHECK(aud->front().high == 65000);  // no EURUSD price leaked in
}

TEST_CASE("BarStore find misses return nullptr", "[barStore]") {
    auto store = makeStore();
    store.registerSeries(minutes{15}, 4);
    store.update(tickAt(minutes{0}, 110000));

    CHECK(store.find("EURUSD", minutes{5}) == nullptr);   // never registered
    CHECK(store.find("AUDUSD", minutes{15}) == nullptr);  // symbol never ticked
}

TEST_CASE("BarStore with no registered series ignores ticks", "[barStore]") {
    auto store = makeStore();
    store.update(tickAt(minutes{0}, 110000));  // must not throw or allocate state
    CHECK(store.find("EURUSD", minutes{15}) == nullptr);
}

TEST_CASE("BarStore rejects unusable range series", "[barStore]") {
    auto store = makeStore();
    CHECK_THROWS_AS(store.registerRangeSeries({0, 100, 8}), std::invalid_argument);
    CHECK_THROWS_AS(store.registerRangeSeries({4, 0, 8}), std::invalid_argument);
    CHECK_THROWS_AS(store.registerRangeSeries({4, 100, 0}), std::invalid_argument);
}

TEST_CASE("BarStore feeds one update into both OHLC and range series",
          "[barStore]") {
    auto store = makeStore();
    store.registerSeries(minutes{15}, 3);
    const rangebar::RangeBarSpec spec{.atrTickWindow = 4,
                                      .atrPercent = 100,
                                      .count = 8};
    store.registerRangeSeries(spec);

    // Four ticks: enough to warm the 4-tick range window; all inside the
    // first 15m OHLC bucket. ONE update call per tick feeds both legs — the
    // loop owners never change for a new bar type.
    store.update(tickAt(minutes{0}, 110000));
    store.update(tickAt(minutes{1}, 110010));
    store.update(tickAt(minutes{2}, 110005));
    store.update(tickAt(minutes{3}, 110008));

    const auto* ohlcSeries = store.find("EURUSD", minutes{15});
    REQUIRE(ohlcSeries != nullptr);
    REQUIRE(ohlcSeries->size() == 1);
    CHECK(ohlcSeries->front().high == 110010);

    const auto* rangeSeries = store.findRange("EURUSD", spec);
    REQUIRE(rangeSeries != nullptr);
    REQUIRE(rangeSeries->size() == 1);  // bar #1 opened on the warm tick
    CHECK(rangeSeries->front().open == 110008);
    CHECK_FALSE(rangeSeries->front().complete);
}

TEST_CASE("BarStore dedups a re-registered range identity keeping the larger "
          "window", "[barStore]") {
    auto store = makeStore();
    store.registerRangeSeries({.atrTickWindow = 1, .atrPercent = 100, .count = 2});
    store.registerRangeSeries({.atrTickWindow = 1, .atrPercent = 100, .count = 4});

    // A 1-tick window is warm immediately with a floored threshold of 1, so
    // alternating prices close a bar every second tick: 12 ticks -> 6 bars,
    // trimmed to the MERGED window of 4 (not the first registration's 2).
    for (int i = 0; i < 12; ++i) {
        store.update(tickAt(seconds{i}, 110000 + (i % 2)));
    }
    const auto* series =
        store.findRange("EURUSD", {.atrTickWindow = 1, .atrPercent = 100, .count = 2});
    REQUIRE(series != nullptr);
    CHECK(series->size() == 4);  // one shared history at the deeper window
}

TEST_CASE("BarStore findRange misses return nullptr", "[barStore]") {
    auto store = makeStore();
    const rangebar::RangeBarSpec spec{.atrTickWindow = 4,
                                      .atrPercent = 100,
                                      .count = 8};
    store.registerRangeSeries(spec);
    store.update(tickAt(minutes{0}, 110000));

    // Identity is (window, percent) — count is ignored, a different window or
    // percent is a different series.
    CHECK(store.findRange("EURUSD", {.atrTickWindow = 5, .atrPercent = 100, .count = 8}) ==
          nullptr);
    CHECK(store.findRange("EURUSD", {.atrTickWindow = 4, .atrPercent = 50, .count = 8}) ==
          nullptr);
    CHECK(store.findRange("AUDUSD", spec) == nullptr);  // symbol never ticked
}

TEST_CASE("BarStore with only range series registered still processes ticks",
          "[barStore]") {
    // Pins the early-return fix: no OHLC series must not short-circuit the
    // range leg.
    auto store = makeStore();
    const rangebar::RangeBarSpec spec{.atrTickWindow = 1,
                                      .atrPercent = 100,
                                      .count = 4};
    store.registerRangeSeries(spec);

    store.update(tickAt(minutes{0}, 110000));

    REQUIRE(store.findRange("EURUSD", spec) != nullptr);
    CHECK(store.findRange("EURUSD", spec)->size() == 1);
    CHECK(store.find("EURUSD", minutes{15}) == nullptr);  // no OHLC state grew
}

TEST_CASE("BarStore keeps per-symbol range histories isolated", "[barStore]") {
    auto store = makeStore();
    const rangebar::RangeBarSpec spec{.atrTickWindow = 1,
                                      .atrPercent = 100,
                                      .count = 4};
    store.registerRangeSeries(spec);

    store.update(tickAt(minutes{0}, 110000, "EURUSD"));
    store.update(tickAt(seconds{30}, 65000, "AUDUSD"));
    store.update(tickAt(minutes{1}, 110100, "EURUSD"));  // closes EURUSD bar 1

    const auto* eur = store.findRange("EURUSD", spec);
    const auto* aud = store.findRange("AUDUSD", spec);
    REQUIRE(eur != nullptr);
    REQUIRE(aud != nullptr);
    CHECK(eur->size() == 1);
    CHECK(eur->front().complete);  // 100-point move >= the floored threshold
    CHECK(aud->size() == 1);
    CHECK_FALSE(aud->front().complete);
    CHECK(aud->front().open == 65000);  // no EURUSD price leaked in
}
