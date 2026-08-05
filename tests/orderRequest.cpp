// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// OrderRequest construction: the C# RequestObject level math translated to
// integer points (entry side by direction, stop adverse, limit favourable,
// pips -> points via symbolScale), plus the deal-reference format IG
// constrains ([A-Za-z0-9_-], max 30 chars) — pinned here because a drifted
// reference silently breaks fill matching in the confirm stream.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>

import liveStrategyRunner;
import orderRequest;
import trade;

namespace {

// 2024-06-26 00:00:00 UTC, same fixture instant as the liveRunner tests.
constexpr std::chrono::system_clock::time_point kTick{
    std::chrono::microseconds{1'719'360'000'000'000LL}};

live::OrderIntent makeIntent(const std::string& symbol,
                             const Direction direction) {
    return live::OrderIntent{
        .strategyName = "StubStrategy",
        .strategyUuid = "f47ac10b-58cc-4372-a567-0e02b2c3d479",
        .symbol = symbol,
        .direction = direction,
        .size = 3,
        .stopDistancePips = 25,
        .limitDistancePips = 50,
        .bid = 110000,
        .ask = 110002,
        .timestamp = kTick,
    };
}

bool validReferenceCharset(const std::string& reference) {
    return std::ranges::all_of(reference, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
    });
}

}  // namespace

TEST_CASE("LONG request enters at ask with stop below and limit above",
          "[orderRequest]") {
    const auto request = live::makeOrderRequest(
        makeIntent("EURUSD", Direction::LONG));  // EURUSD: 10 points per pip
    REQUIRE(request.has_value());

    CHECK(request->pointsPerPip == 10);
    CHECK(request->level == 110002);                  // entry at ask
    CHECK(request->stopLevel == 110002 - 25 * 10);    // stop adverse (below)
    CHECK(request->limitLevel == 110002 + 50 * 10);   // limit favourable (above)
    CHECK(request->spreadPoints == 2);
    CHECK(request->symbol == "EURUSD");
    CHECK(request->size == 3);
    CHECK(request->stopDistancePips == 25);
    CHECK(request->limitDistancePips == 50);
    CHECK(request->timestamp == kTick);
}

TEST_CASE("SHORT request mirrors: enters at bid, stop above, limit below",
          "[orderRequest]") {
    const auto request =
        live::makeOrderRequest(makeIntent("EURUSD", Direction::SHORT));
    REQUIRE(request.has_value());

    CHECK(request->level == 110000);                  // entry at bid
    CHECK(request->stopLevel == 110000 + 25 * 10);    // stop adverse (above)
    CHECK(request->limitLevel == 110000 - 50 * 10);   // limit favourable (below)
    CHECK(request->spreadPoints == 2);
}

TEST_CASE("pip distances scale by the symbol's points-per-pip",
          "[orderRequest]") {
    const auto request = live::makeOrderRequest(
        makeIntent("XAGUSD", Direction::LONG));  // metals: 1000 points per pip
    REQUIRE(request.has_value());
    CHECK(request->pointsPerPip == 1000);
    CHECK(request->stopLevel == 110002 - 25 * 1000);
    CHECK(request->limitLevel == 110002 + 50 * 1000);
}

TEST_CASE("a symbol unknown to symbolScale yields no request",
          "[orderRequest]") {
    CHECK_FALSE(
        live::makeOrderRequest(makeIntent("NOSUCHSYM", Direction::LONG))
            .has_value());
}

TEST_CASE("deal reference fits IG's constraints and format", "[orderRequest]") {
    const std::string reference = live::makeDealReference(
        "f47ac10b-58cc-4372-a567-0e02b2c3d479", Direction::LONG, kTick);

    // First 8 alphanumerics of the UUID (dashes stripped), direction letter,
    // epoch millis of the decision tick.
    CHECK(reference == "f47ac10b-L1719360000000");
    CHECK(reference.size() <= 30);
    CHECK(validReferenceCharset(reference));

    CHECK(live::makeDealReference("f47ac10b-58cc-4372-a567-0e02b2c3d479",
                                  Direction::SHORT, kTick)
          == "f47ac10b-S1719360000000");
    // Different decision ticks mint different references (the idempotency
    // token must be unique per placement attempt).
    CHECK(live::makeDealReference("f47ac10b-58cc-4372-a567-0e02b2c3d479",
                                  Direction::LONG,
                                  kTick + std::chrono::milliseconds{1})
          != reference);
}

TEST_CASE("makeOrderRequest carries the minted deal reference",
          "[orderRequest]") {
    const auto request =
        live::makeOrderRequest(makeIntent("EURUSD", Direction::LONG));
    REQUIRE(request.has_value());
    CHECK(request->dealReference == "f47ac10b-L1719360000000");
    CHECK(validReferenceCharset(request->dealReference));
}
