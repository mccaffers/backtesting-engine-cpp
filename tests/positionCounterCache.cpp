// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// PositionCountCache — the MAX_OPEN_TRADES cap's per-thread count cache.
// Clock-free by design (`now`/`freshUntil` are injected), so expiry and the
// invalidate-after-open contract are pinned here without waiting on a TTL.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

import redisPositionCounter;

namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

TEST_CASE("a cached count is served only while fresh", "[positionCounter]") {
    live::PositionCountCache cache;
    const Clock::time_point t0{};
    const auto ttl = std::chrono::seconds{15};

    CHECK_FALSE(cache.get("u-eur", t0).has_value());

    cache.put("u-eur", 2, t0 + ttl);
    CHECK(cache.get("u-eur", t0) == 2);
    CHECK(cache.get("u-eur", t0 + ttl - std::chrono::milliseconds{1}) == 2);

    // freshUntil is EXCLUSIVE: at exactly the deadline the value is stale.
    CHECK_FALSE(cache.get("u-eur", t0 + ttl).has_value());
    CHECK_FALSE(cache.get("u-eur", t0 + ttl * 2).has_value());
}

TEST_CASE("invalidate forces the next read back to Redis",
          "[positionCounter]") {
    // The L4 regression: after this worker's own accepted open moved PL#,
    // serving the pre-open count for the rest of the TTL let a second
    // direction through MAX_OPEN_TRADES (the trade lock is per-direction
    // and never serialised the two). brokerOrderSink invalidates on every
    // accepted open / successful close; a fresh get() must then miss.
    live::PositionCountCache cache;
    const Clock::time_point t0{};
    const auto freshUntil = t0 + std::chrono::seconds{15};

    cache.put("u-eur", 0, freshUntil);
    REQUIRE(cache.get("u-eur", t0) == 0);

    cache.invalidate("u-eur");
    CHECK_FALSE(cache.get("u-eur", t0).has_value());

    // Re-population after the forced miss behaves like any fresh entry.
    cache.put("u-eur", 1, freshUntil);
    CHECK(cache.get("u-eur", t0) == 1);
}

TEST_CASE("entries are isolated per strategy uuid", "[positionCounter]") {
    live::PositionCountCache cache;
    const Clock::time_point t0{};
    const auto freshUntil = t0 + std::chrono::seconds{15};

    cache.put("u-eur", 1, freshUntil);
    cache.put("u-gbp", 3, freshUntil);

    cache.invalidate("u-eur");
    CHECK_FALSE(cache.get("u-eur", t0).has_value());
    CHECK(cache.get("u-gbp", t0) == 3);  // untouched

    // Invalidating a uuid nobody cached is a harmless no-op.
    cache.invalidate("u-unknown");
    CHECK(cache.get("u-gbp", t0) == 3);
}
