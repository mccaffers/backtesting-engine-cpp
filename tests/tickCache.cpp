// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import priceData;
import tickCache;

using std::chrono::system_clock;
using tick_cache::Superset;
using tick_cache::TickCache;

namespace {

// Deliberately small numbers unrelated to the production ladder: these tests
// pin the slicing/caching machinery, never the currently-configured windows.
constexpr int kMonths = 4;

const system_clock::time_point kSnapshot{
    std::chrono::sys_days{std::chrono::year{2026} / 5 / 1}};

// boundaries[m] = m "months" before the snapshot. The cache and slicer only
// ever binary-search against these instants, so a fixed 30-day month keeps the
// fixture simple without loosening the tests.
std::vector<system_clock::time_point> makeBoundaries(const int months) {
    std::vector<system_clock::time_point> boundaries(months + 1);
    for (int m = 0; m <= months; ++m) {
        boundaries[m] = kSnapshot - std::chrono::days{30 * m};
    }
    return boundaries;
}

PriceData tickAt(const system_clock::time_point ts) {
    return PriceData(100, 99, ts, "EURUSD");
}

// A superset whose ticks sit at known offsets from the boundaries: one tick
// exactly ON each boundary below the snapshot, and one strictly inside each
// month, so inclusive/exclusive edges are directly observable.
std::shared_ptr<const Superset> makeSuperset(const int months) {
    auto boundaries = makeBoundaries(months);
    std::vector<PriceData> ticks;
    for (int m = months; m >= 1; --m) {
        ticks.push_back(tickAt(boundaries[m]));                          // on boundary
        ticks.push_back(tickAt(boundaries[m] + std::chrono::days{10}));  // inside month
    }
    ticks.push_back(tickAt(kSnapshot - std::chrono::microseconds{1}));   // just before T
    return std::make_shared<const Superset>(Superset{std::move(ticks), std::move(boundaries)});
}

}  // namespace

TEST_CASE("sliceForWindow honours inclusive-lower / exclusive-upper bounds", "[tickCache]") {
    const auto superset = makeSuperset(kMonths);
    const auto& ticks = superset->ticks;
    const auto& boundaries = superset->boundaries;

    SECTION("a shifted window includes its lower boundary tick and excludes its upper") {
        const auto slice = tick_cache::sliceForWindow(superset, 2, 2);
        REQUIRE(slice.count > 0);
        // First tick in [boundaries[4], boundaries[2]) is the one exactly on
        // boundaries[4]; the tick exactly on boundaries[2] belongs to the NEXT
        // window up, so the slice stops just before it.
        CHECK(ticks[slice.begin].timestamp == boundaries[4]);
        CHECK(ticks[slice.begin + slice.count - 1].timestamp <
              boundaries[2]);
        CHECK(ticks[slice.begin + slice.count].timestamp == boundaries[2]);
    }

    SECTION("offset 0 leaves the upper end open to the superset's last tick") {
        const auto slice = tick_cache::sliceForWindow(superset, 2, 0);
        REQUIRE(slice.count > 0);
        CHECK(ticks[slice.begin].timestamp == boundaries[2]);
        CHECK(slice.begin + slice.count == ticks.size());
    }

    SECTION("the full-depth window is the whole buffer") {
        const auto slice = tick_cache::sliceForWindow(superset, kMonths, 0);
        CHECK(slice.begin == 0);
        CHECK(slice.count == ticks.size());
    }

    SECTION("a window with no ticks is empty, not an error") {
        auto boundaries2 = makeBoundaries(2);
        // Only one tick, in the most recent month: the (1,1) window is empty.
        std::vector<PriceData> sparse{tickAt(boundaries2[1] + std::chrono::days{1})};
        const auto sparseSet = std::make_shared<const Superset>(
            Superset{std::move(sparse), std::move(boundaries2)});
        const auto slice = tick_cache::sliceForWindow(sparseSet, 1, 1);
        CHECK(slice.count == 0);
    }

    SECTION("windows deeper than the superset and degenerate windows throw") {
        CHECK_THROWS_AS(tick_cache::sliceForWindow(superset, kMonths, 1), std::logic_error);
        CHECK_THROWS_AS(tick_cache::sliceForWindow(superset, 0, 0), std::logic_error);
        CHECK_THROWS_AS(tick_cache::sliceForWindow(superset, 1, -1), std::logic_error);
    }

    SECTION("the slice keeps the superset alive on its own") {
        auto slice = tick_cache::sliceForWindow(superset, 2, 0);
        const auto* raw = slice.superset.get();
        CHECK(raw == superset.get());
        CHECK(slice.superset.use_count() >= 2);
    }
}

namespace {

// Test harness around TickCache: counting loaders and a hand-cranked clock, so
// hits/misses/TTL are observable without QuestDB or sleeping.
struct CacheFixture {
    int supersetLoads = 0;
    int windowLoads = 0;
    int beforeLoads = 0;
    int lastMonthsRequested = 0;
    int sequence = 0;          // increments on every hook/loader call
    int beforeLoadSeq = -1;
    int loaderSeq = -1;
    bool throwOnLoad = false;
    std::chrono::steady_clock::time_point now{};

    TickCache makeCache(const std::chrono::seconds ttl = std::chrono::seconds{600},
                        const std::size_t maxSupersets = 1) {
        return TickCache(
            [this](const std::string&, const int months) {
                if (throwOnLoad) {
                    throw std::runtime_error("loader failed");
                }
                ++supersetLoads;
                loaderSeq = ++sequence;
                lastMonthsRequested = months;
                Superset superset;
                superset.boundaries = makeBoundaries(months);
                superset.ticks.push_back(tickAt(kSnapshot - std::chrono::days{1}));
                return superset;
            },
            [this](const std::string&, const int, const int) {
                ++windowLoads;
                loaderSeq = ++sequence;
                return std::vector<PriceData>{tickAt(kSnapshot - std::chrono::days{400})};
            },
            ttl, maxSupersets, kMonths,
            [this] { return now; });
    }

    std::function<void()> beforeLoad() {
        return [this] {
            ++beforeLoads;
            beforeLoadSeq = ++sequence;
        };
    }
};

}  // namespace

TEST_CASE("TickCache serves repeat windows from one superset load", "[tickCache]") {
    CacheFixture fx;
    auto cache = fx.makeCache();

    const auto first = cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);
    CHECK(fx.beforeLoads == 1);
    // beforeLoad must fire BEFORE the load it announces, so the drain loop's
    // quiesce provably precedes the eviction inside.
    CHECK(fx.beforeLoadSeq < fx.loaderSeq);
    // The first load already requests the default depth, not the window's.
    CHECK(fx.lastMonthsRequested == kMonths);

    // Every window that fits the superset is a pure hit: no loader, no hook.
    const std::vector<std::pair<int, int>> windows{{2, 0}, {2, 2}, {1, 3}, {kMonths, 0}};
    for (const auto& [last, offset] : windows) {
        const auto slice = cache.get("EURUSD", last, offset, fx.beforeLoad());
        CHECK(slice.superset == first.superset);
    }
    CHECK(fx.supersetLoads == 1);
    CHECK(fx.windowLoads == 0);
    CHECK(fx.beforeLoads == 1);
}

TEST_CASE("TickCache reloads after the TTL expires", "[tickCache]") {
    CacheFixture fx;
    auto cache = fx.makeCache(std::chrono::seconds{600});

    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    fx.now += std::chrono::seconds{599};
    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);

    fx.now += std::chrono::seconds{1};  // exactly TTL old now — stale
    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 2);
    CHECK(fx.beforeLoads == 2);
}

TEST_CASE("TickCache evicts the resident superset when a new key arrives", "[tickCache]") {
    CacheFixture fx;
    auto cache = fx.makeCache(std::chrono::seconds{600}, /*maxSupersets=*/1);

    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    cache.get("USDJPY", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 2);

    // EURUSD was evicted to make room, so it must load again.
    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 3);
    CHECK(fx.beforeLoads == 3);
}

TEST_CASE("TickCache sends too-deep windows to the ad-hoc loader without evicting", "[tickCache]") {
    CacheFixture fx;
    auto cache = fx.makeCache();

    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);

    // Deeper than the resident 4-month superset: ad-hoc window load, resident
    // superset untouched.
    const auto deep = cache.get("EURUSD", kMonths, 3, fx.beforeLoad());
    CHECK(fx.windowLoads == 1);
    CHECK(fx.supersetLoads == 1);
    CHECK(fx.beforeLoads == 2);
    CHECK(deep.begin == 0);
    CHECK(deep.count == deep.superset->ticks.size());

    // ...and the resident superset still serves fitting windows as hits.
    cache.get("EURUSD", 2, 2, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);
    CHECK(fx.beforeLoads == 2);
}

TEST_CASE("TickCache sizes the first load to the deepest of default and requested", "[tickCache]") {
    CacheFixture fx;
    auto cache = fx.makeCache();

    // Deeper than the default: the superset grows to fit rather than falling
    // to the ad-hoc path, so later shallow windows still hit.
    cache.get("EURUSD", kMonths, 2, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);
    CHECK(fx.lastMonthsRequested == kMonths + 2);

    cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);
}

TEST_CASE("TickCache propagates loader failures and retries on the next get", "[tickCache]") {
    CacheFixture fx;
    auto cache = fx.makeCache();

    fx.throwOnLoad = true;
    CHECK_THROWS_AS(cache.get("EURUSD", 2, 0, fx.beforeLoad()), std::runtime_error);

    fx.throwOnLoad = false;
    const auto slice = cache.get("EURUSD", 2, 0, fx.beforeLoad());
    CHECK(fx.supersetLoads == 1);
    CHECK(slice.superset != nullptr);
}
