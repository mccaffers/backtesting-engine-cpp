// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// The lock KEY FORMAT is a wire contract shared with the C# engine's
// TradeLocks (LOCK#<strategyId>#<direction>) — a drifted prefix or separator
// would silently split the lock space between the two engines, so it is
// pinned here without needing a Redis server. The SET NX PX behaviour itself
// lives behind RedisOperations and is exercised in the live smoke test.

#include <catch2/catch_test_macros.hpp>

#include "shared/redis/tradeLocks.hpp"

TEST_CASE("lockKey matches the C# TradeLocks key format", "[tradeLocks]") {
    CHECK(redis_locks::lockKey("abc-123", "LONG") == "LOCK#abc-123#LONG");
    CHECK(redis_locks::lockKey("abc-123", "SHORT") == "LOCK#abc-123#SHORT");
}

TEST_CASE("lockKey keeps strategy UUIDs distinct", "[tradeLocks]") {
    CHECK(redis_locks::lockKey("a", "LONG") != redis_locks::lockKey("b", "LONG"));
    CHECK(redis_locks::lockKey("a", "LONG") != redis_locks::lockKey("a", "SHORT"));
}
