// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// The live position book codecs: the PO# payload the order channel writes
// must round-trip through the decoder the position feed reads (a drift
// silently empties every worker's book), and the record -> BookedPosition
// mapping must reverse the trade-size modifier so seeded trades reflect the
// actual broker position. Redis itself is not involved — same codec-only
// precedent as tests/positionManager.cpp.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "shared/redis/positionManager.hpp"

import igMarkets;
import liveStrategyRunner;
import marketDefinitions;
import orderChannel;
import orderRequest;
import redisPositionFeed;
import trade;

namespace {

constexpr std::chrono::system_clock::time_point kTick{
    std::chrono::microseconds{1'719'360'000'000'000LL}};

live::OrderRequest makeRequest() {
    const auto request = live::makeOrderRequest(live::OrderIntent{
        .strategyName = "StubStrategy",
        .strategyUuid = "u-eur",
        .symbol = "EURUSD",
        .direction = Direction::LONG,
        .size = 3,
        .stopDistancePips = 25,
        .limitDistancePips = 50,
        .bid = 110000,
        .ask = 110002,
        .timestamp = kTick,
    });
    REQUIRE(request.has_value());
    return *request;
}

redis_positions::PositionRecord makeRecord() {
    redis_positions::PositionRecord record;
    record.dealId = "DIAAA-1";
    record.dealReference = "IGREF-1";
    record.symbol = "EURUSD";
    record.direction = "BUY";
    record.size = 1.5;
    record.level = 110002;
    record.openedAtMicros = 1'719'360'000'000'000LL;
    return record;
}

}  // namespace

TEST_CASE("the PO# payload round-trips through decodePositionRecord",
          "[positionBook]") {
    const live::OrderRequest request = makeRequest();
    const ig::TradeOpenObj order{
        .currencyCode = "USD",
        .epic = "TEST.EPIC.MINI.IP",
        .direction = "BUY",
        .size = 1.5,
        .stopDistance = 25,
        .limitDistance = 50,
        .dealReference = request.dealReference,
    };
    const std::string payload = live::buildPositionPayload(
        request, order, "IGREF-1", "DIAAA-1");

    const auto record = redis_positions::decodePositionRecord(payload);
    REQUIRE(record.has_value());
    CHECK(record->dealId == "DIAAA-1");
    CHECK(record->dealReference == "IGREF-1");
    CHECK(record->symbol == "EURUSD");
    CHECK(record->epic == "TEST.EPIC.MINI.IP");
    CHECK(record->direction == "BUY");
    CHECK(record->size == 1.5);
    CHECK(record->level == 110002);
    CHECK(record->stopLevel == 110002 - 250);
    CHECK(record->limitLevel == 110002 + 500);
    CHECK(record->strategyId == "u-eur");
    CHECK(record->strategyName == "StubStrategy");
    CHECK(record->openedAtMicros == 1'719'360'000'000'000LL);
}

TEST_CASE("decodePositionRecord tolerates missing optionals and rejects "
          "garbage",
          "[positionBook]") {
    // Only identity, symbol and direction are load-bearing — a
    // producer-rewritten payload may drop the rest.
    const auto minimal = redis_positions::decodePositionRecord(
        R"({"dealReference":"R1","symbol":"EURUSD","direction":"SELL"})");
    REQUIRE(minimal.has_value());
    CHECK(minimal->dealId.empty());
    CHECK(minimal->size == 0.0);
    CHECK(minimal->level == 0);

    CHECK_FALSE(redis_positions::decodePositionRecord(
                    R"({"symbol":"EURUSD","direction":"BUY"})")  // no reference
                    .has_value());
    CHECK_FALSE(redis_positions::decodePositionRecord(
                    R"({"dealReference":"R1","direction":"BUY"})")  // no symbol
                    .has_value());
    CHECK_FALSE(redis_positions::decodePositionRecord(
                    R"({"dealReference":"R1","symbol":"EURUSD"})")  // no direction
                    .has_value());
    CHECK_FALSE(redis_positions::decodePositionRecord("not json").has_value());
    CHECK_FALSE(redis_positions::decodePositionRecord("[]").has_value());
    CHECK_FALSE(redis_positions::decodePositionRecord("").has_value());
}

TEST_CASE("toBookedPosition maps BUY/SELL and reverses the size modifier",
          "[positionBook]") {
    // A market with a 0.5 modifier: broker 1.5 units = 3 engine lots.
    constexpr live::MarketDefinition halved{
        .symbol = "EURUSD",
        .igMarketId = "EURUSD",
        .epicCfd = "TEST.EPIC.CFD.IP",
        .epicMini = "TEST.EPIC.MINI.IP",
        .currency = "USD",
        .polygonIdentifier = "",
        .polygonScale = 0,
        .type = live::MarketType::Forex,
        .tradeSizeModifier = 0.5,
    };
    const auto buy = live::toBookedPosition(makeRecord(), &halved);
    REQUIRE(buy.has_value());
    CHECK(buy->direction == Direction::LONG);
    CHECK(buy->brokerSize == 1.5);   // verbatim — what a close must send
    CHECK(buy->engineSize == 3);     // 1.5 / 0.5
    CHECK(buy->level == 110002);
    CHECK(buy->dealId == "DIAAA-1");
    CHECK(buy->dealReference == "IGREF-1");
    CHECK(buy->openedAt == kTick);

    auto sellRecord = makeRecord();
    sellRecord.direction = "SELL";
    const auto sell = live::toBookedPosition(sellRecord, &halved);
    REQUIRE(sell.has_value());
    CHECK(sell->direction == Direction::SHORT);

    // Symbol dropped from the market table: size passes through unscaled.
    const auto unscaled = live::toBookedPosition(makeRecord(), nullptr);
    REQUIRE(unscaled.has_value());
    CHECK(unscaled->engineSize == 2);  // llround(1.5 / 1.0)

    // An unaddressable direction must not enter the book.
    auto badRecord = makeRecord();
    badRecord.direction = "LONG";  // engine vocabulary, not the broker's
    CHECK_FALSE(live::toBookedPosition(badRecord, &halved).has_value());
}
