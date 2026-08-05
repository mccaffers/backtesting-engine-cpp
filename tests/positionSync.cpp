// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// positionSync — the pure half of the IG position producer (the C#
// igmarkets_positions port): the /positions JSON decode, IG's createdDate
// parse, the update-TTL weekend rule, and the fresh-record/update mutations.
// All exercised without Redis or HTTP, with the price scale INJECTED — the
// machinery is under test, not the current symbolScale/marketDefinitions
// tables. The one table check (findMarketByEpicMini) is self-consistency:
// every market must be reachable from its own mini epic, whatever the table
// currently lists.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "shared/redis/positionManager.hpp"

import positionSync;
import marketDefinitions;

using namespace std::chrono;

namespace {

std::int64_t microsAt(const sys_days day, const hours h, const minutes m,
                      const seconds s, const milliseconds ms) {
    return duration_cast<microseconds>((day + h + m + s + ms).time_since_epoch())
        .count();
}

positions::IgPosition samplePosition() {
    positions::IgPosition position;
    position.epic = "CS.D.CFPGOLD.CFP.IP";
    position.dealReference = "ref-1";
    position.dealId = "DIAAAA";
    position.direction = "BUY";
    position.createdDate = "2026/07/10 09:30:00:250";
    position.dealSize = 1.5;
    position.openLevel = 2345.678;
    position.stopLevel = 2340.0;
    position.limitLevel = 0.0;  // IG null decoded as 0
    return position;
}

}  // namespace

// ---- updateTtl: the Friday-night weekend hold ------------------------------

TEST_CASE("updateTtl is 10 minutes on an ordinary weekday",
          "[positionSync]") {
    // 2026-07-09 is a Thursday, 2026-07-11 a Saturday.
    CHECK(positions::updateTtl(sys_days{2026y / 7 / 9} + 21h + 56min)
          == minutes{10});
    CHECK(positions::updateTtl(sys_days{2026y / 7 / 11} + 21h + 57min)
          == minutes{10});
}

TEST_CASE("updateTtl holds Friday 21:55:00-21:59:59 UTC over the weekend",
          "[positionSync]") {
    constexpr auto weekendHold = days{2} + hours{2};
    const sys_days friday{2026y / 7 / 10};  // a Friday
    CHECK(positions::updateTtl(friday + 21h + 54min + 59s) == minutes{10});
    CHECK(positions::updateTtl(friday + 21h + 55min) == weekendHold);
    CHECK(positions::updateTtl(friday + 21h + 59min + 59s) == weekendHold);
    CHECK(positions::updateTtl(friday + 22h) == minutes{10});
    // Same minutes on other Friday hours stay ordinary.
    CHECK(positions::updateTtl(friday + 20h + 57min) == minutes{10});
}

// ---- parseCreatedDateMicros: IG's "yyyy/MM/dd HH:mm:ss:fff" ----------------

TEST_CASE("parseCreatedDateMicros decodes IG's createdDate as UTC micros",
          "[positionSync]") {
    CHECK(positions::parseCreatedDateMicros("2026/07/10 21:55:00:123", 42)
          == microsAt(sys_days{2026y / 7 / 10}, 21h, 55min, 0s,
                      milliseconds{123}));
    CHECK(positions::parseCreatedDateMicros("1970/01/01 00:00:00:000", 42)
          == 0);
}

TEST_CASE("parseCreatedDateMicros falls back on anything malformed",
          "[positionSync]") {
    constexpr std::int64_t fallback = 42;
    CHECK(positions::parseCreatedDateMicros("", fallback) == fallback);
    CHECK(positions::parseCreatedDateMicros("not a date", fallback)
          == fallback);
    // Truncated (no millis).
    CHECK(positions::parseCreatedDateMicros("2026/07/10 21:55:00", fallback)
          == fallback);
    // Wrong separators (ISO dashes).
    CHECK(positions::parseCreatedDateMicros("2026-07-10 21:55:00:123",
                                            fallback)
          == fallback);
    // Out-of-range fields.
    CHECK(positions::parseCreatedDateMicros("2026/13/10 21:55:00:123",
                                            fallback)
          == fallback);
    CHECK(positions::parseCreatedDateMicros("2026/02/30 21:55:00:123",
                                            fallback)
          == fallback);
    CHECK(positions::parseCreatedDateMicros("2026/07/10 24:00:00:000",
                                            fallback)
          == fallback);
    // Non-digits in a field.
    CHECK(positions::parseCreatedDateMicros("2026/07/10 21:5x:00:123",
                                            fallback)
          == fallback);
}

// ---- decodeAccountPositions: the C# AccountPositions shape -----------------

TEST_CASE("decodeAccountPositions reads the nested market/position fields",
          "[positionSync]") {
    const std::string body = R"({
        "positions": [{
            "position": {
                "contractSize": 1.0,
                "createdDate": "2026/07/10 09:30:00:250",
                "dealId": "DIAAAA",
                "dealSize": 1.5,
                "dealReference": "ref-1",
                "direction": "BUY",
                "limitLevel": null,
                "openLevel": 2345.678,
                "currency": "GBP",
                "controlledRisk": false,
                "stopLevel": 2340.0
            },
            "market": {
                "instrumentName": "Spot Gold",
                "epic": "CS.D.CFPGOLD.CFP.IP",
                "bid": 2345.5,
                "offer": 2345.9
            }
        }]
    })";
    const auto decoded = positions::decodeAccountPositions(body);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 1);
    const positions::IgPosition& position = decoded->front();
    CHECK(position.epic == "CS.D.CFPGOLD.CFP.IP");
    CHECK(position.dealReference == "ref-1");
    CHECK(position.dealId == "DIAAAA");
    CHECK(position.direction == "BUY");
    CHECK(position.createdDate == "2026/07/10 09:30:00:250");
    CHECK(position.dealSize == 1.5);
    CHECK(position.openLevel == 2345.678);
    CHECK(position.stopLevel == 2340.0);
    CHECK(position.limitLevel == 0.0);  // null -> 0, the C# `?? 0`
}

TEST_CASE("decodeAccountPositions tolerates missing sub-objects per entry",
          "[positionSync]") {
    const auto decoded = positions::decodeAccountPositions(
        R"({"positions":[{"position":{"dealReference":"ref-2"}},{}]})");
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 2);
    CHECK(decoded->at(0).dealReference == "ref-2");
    CHECK(decoded->at(0).epic.empty());  // no market object
    CHECK(decoded->at(1).dealReference.empty());
}

TEST_CASE("decodeAccountPositions treats a null/absent positions list as an "
          "empty book",
          "[positionSync]") {
    const auto absent = positions::decodeAccountPositions("{}");
    REQUIRE(absent.has_value());
    CHECK(absent->empty());
    const auto null = positions::decodeAccountPositions(R"({"positions":null})");
    REQUIRE(null.has_value());
    CHECK(null->empty());
    const auto empty =
        positions::decodeAccountPositions(R"({"positions":[]})");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("decodeAccountPositions rejects undecodable bodies",
          "[positionSync]") {
    CHECK_FALSE(positions::decodeAccountPositions("not json").has_value());
    CHECK_FALSE(positions::decodeAccountPositions("[]").has_value());
    CHECK_FALSE(
        positions::decodeAccountPositions(R"({"positions":42})").has_value());
    CHECK_FALSE(positions::decodeAccountPositions(R"({"positions":["x"]})")
                    .has_value());
}

// ---- makeFreshRecord: the C# Save() ----------------------------------------

TEST_CASE("makeFreshRecord scales prices with the injected multiplier",
          "[positionSync]") {
    const auto record = positions::makeFreshRecord(
        samplePosition(), "XAUUSD", "uuid-1", "Fvg", 1000, 99);
    CHECK(record.dealId == "DIAAAA");
    CHECK(record.dealReference == "ref-1");
    CHECK(record.symbol == "XAUUSD");
    CHECK(record.epic == "CS.D.CFPGOLD.CFP.IP");
    CHECK(record.direction == "BUY");
    CHECK(record.size == 1.5);
    CHECK(record.level == 2345678);
    CHECK(record.stopLevel == 2340000);
    CHECK(record.limitLevel == 0);
    CHECK(record.strategyId == "uuid-1");
    CHECK(record.strategyName == "Fvg");
    CHECK(record.openedAtMicros
          == microsAt(sys_days{2026y / 7 / 10}, 9h, 30min, 0s,
                      milliseconds{250}));
}

TEST_CASE("makeFreshRecord rounds half away from zero, like the C# ScalePrice",
          "[positionSync]") {
    // 0.25 is binary-exact, so 0.25 * 10 is EXACTLY 2.5 — a true halfway
    // case (a banker's-rounding encoder would write 2). Away-from-zero is
    // the C# MidpointRounding the wire contract expects.
    positions::IgPosition position = samplePosition();
    position.openLevel = 0.25;    // * 10 = 2.5 -> 3
    position.stopLevel = -0.25;   // * 10 = -2.5 -> -3 (away from zero)
    position.limitLevel = 0.75;   // * 10 = 7.5 -> 8
    const auto record =
        positions::makeFreshRecord(position, "EURUSD", "s", "n", 10, 0);
    CHECK(record.level == 3);
    CHECK(record.stopLevel == -3);
    CHECK(record.limitLevel == 8);
}

TEST_CASE("makeFreshRecord writes level 0 when there is no price scale",
          "[positionSync]") {
    const auto record = positions::makeFreshRecord(samplePosition(), "XAUUSD",
                                                   "s", "n", 0, 0);
    CHECK(record.level == 0);
    CHECK(record.stopLevel == 0);
    CHECK(record.limitLevel == 0);
}

TEST_CASE("makeFreshRecord stamps openedAt with the fallback when "
          "createdDate is malformed",
          "[positionSync]") {
    positions::IgPosition position = samplePosition();
    position.createdDate = "garbage";
    constexpr std::int64_t nowMicros = 1234567890;
    const auto record = positions::makeFreshRecord(position, "XAUUSD", "s",
                                                   "n", 1000, nowMicros);
    CHECK(record.openedAtMicros == nowMicros);
}

// ---- applyBrokerUpdate: the C# Update() ------------------------------------

TEST_CASE("applyBrokerUpdate fills a missing strategy attribution",
          "[positionSync]") {
    redis_positions::PositionRecord record;
    record.strategyId = "";
    positions::applyBrokerUpdate(record, samplePosition(), "uuid-1");
    CHECK(record.strategyId == "uuid-1");

    record.strategyId = "Unknown";
    positions::applyBrokerUpdate(record, samplePosition(), "uuid-2");
    CHECK(record.strategyId == "uuid-2");
}

TEST_CASE("applyBrokerUpdate keeps a real strategy attribution",
          "[positionSync]") {
    redis_positions::PositionRecord record;
    record.strategyId = "uuid-original";
    positions::applyBrokerUpdate(record, samplePosition(), "uuid-receipt");
    CHECK(record.strategyId == "uuid-original");
}

TEST_CASE("applyBrokerUpdate refreshes the dealId only when IG reports one",
          "[positionSync]") {
    redis_positions::PositionRecord record;
    record.dealId = "DI-OLD";
    positions::IgPosition position = samplePosition();
    position.dealId = "";
    positions::applyBrokerUpdate(record, position, "s");
    CHECK(record.dealId == "DI-OLD");

    position.dealId = "DI-NEW";
    positions::applyBrokerUpdate(record, position, "s");
    CHECK(record.dealId == "DI-NEW");
}

// ---- findMarketByEpicMini: reverse lookup self-consistency -----------------

TEST_CASE("every market is reachable from its own mini epic",
          "[positionSync]") {
    for (const live::MarketDefinition& market : live::kMarkets) {
        INFO("epicMini not found: " << market.epicMini);
        const live::MarketDefinition* found =
            live::findMarketByEpicMini(market.epicMini);
        REQUIRE(found != nullptr);
        CHECK(found->epicMini == market.epicMini);
    }
}

TEST_CASE("an unknown epic finds no market", "[positionSync]") {
    CHECK(live::findMarketByEpicMini("IX.D.NOTREAL.IFS.IP") == nullptr);
    CHECK(live::findMarketByEpicMini("") == nullptr);
}
