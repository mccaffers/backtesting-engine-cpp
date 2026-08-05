// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// The position KEY FORMATS and the PL# list encoding are a wire contract
// shared with the C# engine's PositionManager and the external position
// producer (PL#<strategyId> / PO#<dealId> / PH#<dealId>, list = JSON string
// array) — drift would silently split the position space between the
// programs, so both are pinned here without needing a Redis server. The
// Redis behaviour itself lives behind RedisOperations, same as TradeLocks.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "shared/redis/positionManager.hpp"

TEST_CASE("position keys match the C# PositionManager prefixes",
          "[positionManager]") {
    CHECK(redis_positions::positionListKey("uuid-1") == "PL#uuid-1");
    CHECK(redis_positions::positionKey("deal-9") == "PO#deal-9");
    CHECK(redis_positions::historyPositionKey("deal-9") == "PH#deal-9");
}

TEST_CASE("the three key families never collide for the same id",
          "[positionManager]") {
    const std::string id = "abc";
    CHECK(redis_positions::positionListKey(id) != redis_positions::positionKey(id));
    CHECK(redis_positions::positionKey(id)
          != redis_positions::historyPositionKey(id));
    CHECK(redis_positions::positionListKey(id)
          != redis_positions::historyPositionKey(id));
}

TEST_CASE("position list codec round-trips the C# JSON array shape",
          "[positionManager]") {
    CHECK(redis_positions::encodePositionList({}) == "[]");
    CHECK(redis_positions::encodePositionList({"d1", "d2"})
          == R"(["d1","d2"])");

    const std::vector<std::string> dealIds{"deal-1", "deal-2", "deal-3"};
    const auto decoded = redis_positions::decodePositionList(
        redis_positions::encodePositionList(dealIds));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == dealIds);
}

TEST_CASE("decodePositionList accepts an empty array and preserves order",
          "[positionManager]") {
    const auto empty = redis_positions::decodePositionList("[]");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const auto ordered = redis_positions::decodePositionList(R"(["b","a"])");
    REQUIRE(ordered.has_value());
    CHECK(*ordered == std::vector<std::string>{"b", "a"});
}

TEST_CASE("decodePositionList rejects anything but a JSON string array",
          "[positionManager]") {
    CHECK_FALSE(redis_positions::decodePositionList("not json").has_value());
    CHECK_FALSE(redis_positions::decodePositionList("{}").has_value());
    CHECK_FALSE(redis_positions::decodePositionList("null").has_value());
    CHECK_FALSE(redis_positions::decodePositionList("\"d1\"").has_value());
    CHECK_FALSE(redis_positions::decodePositionList("[1, 2]").has_value());
    CHECK_FALSE(redis_positions::decodePositionList(R"(["d1", 2])").has_value());
}

TEST_CASE("encodePositionRecord writes the orderChannel/C# PO# wire shape",
          "[positionManager]") {
    // Exact-string pin: the field ORDER is part of the contract (the C#
    // JsonSerializer and buildPositionPayload both write it this way), so an
    // alphabetising encoder would be a wire drift even with equal content.
    const redis_positions::PositionRecord record{
        .dealId = "DIAAAA",
        .dealReference = "ref-1",
        .symbol = "XAUUSD",
        .epic = "CS.D.CFPGOLD.CFP.IP",
        .direction = "BUY",
        .size = 1.5,
        .level = 2345678,
        .stopLevel = 2340000,
        .limitLevel = 0,
        .strategyId = "uuid-1",
        .strategyName = "Fvg",
        .openedAtMicros = 1720000000000000,
    };
    CHECK(redis_positions::encodePositionRecord(record)
          == R"({"dealId":"DIAAAA","dealReference":"ref-1","symbol":"XAUUSD",)"
             R"("epic":"CS.D.CFPGOLD.CFP.IP","direction":"BUY","size":1.5,)"
             R"("level":2345678,"stopLevel":2340000,"limitLevel":0,)"
             R"("strategyId":"uuid-1","strategyName":"Fvg",)"
             R"("openedAt":1720000000000000})");
}

TEST_CASE("encodePositionRecord round-trips through decodePositionRecord",
          "[positionManager]") {
    redis_positions::PositionRecord record;
    record.dealId = "DI-1";
    record.dealReference = "ref-2";
    record.symbol = "EURUSD";
    record.epic = "CS.D.EURUSD.MINI.IP";
    record.direction = "SELL";
    record.size = 2.0;
    record.level = 110001;
    record.stopLevel = 110500;
    record.limitLevel = 109000;
    record.strategyId = "uuid-9";
    record.strategyName = "OhlcBreakout";
    record.openedAtMicros = 1234567890123456;

    const auto decoded = redis_positions::decodePositionRecord(
        redis_positions::encodePositionRecord(record));
    REQUIRE(decoded.has_value());
    CHECK(decoded->dealId == record.dealId);
    CHECK(decoded->dealReference == record.dealReference);
    CHECK(decoded->symbol == record.symbol);
    CHECK(decoded->epic == record.epic);
    CHECK(decoded->direction == record.direction);
    CHECK(decoded->size == record.size);
    CHECK(decoded->level == record.level);
    CHECK(decoded->stopLevel == record.stopLevel);
    CHECK(decoded->limitLevel == record.limitLevel);
    CHECK(decoded->strategyId == record.strategyId);
    CHECK(decoded->strategyName == record.strategyName);
    CHECK(decoded->openedAtMicros == record.openedAtMicros);
}

TEST_CASE("decodeDealReceipt reads the buildDealReceipt/C# DealReceipt shape",
          "[positionManager]") {
    // The literal mirrors orderChannel::buildDealReceipt — the producer only
    // consumes the strategy attribution and the broker dealId.
    const auto receipt = redis_positions::decodeDealReceipt(
        R"({"id":"DealId#ref-1","sort":"XAUUSD","date":"2026-07-10T09:00:00Z",)"
        R"("dealReference":"ref-1","strategyId":"uuid-1","dealId":"DIAAAA",)"
        R"("strategyName":"Fvg"})");
    REQUIRE(receipt.has_value());
    CHECK(receipt->strategyId == "uuid-1");
    CHECK(receipt->dealId == "DIAAAA");
    CHECK(receipt->strategyName == "Fvg");
}

TEST_CASE("decodeDealReceipt tolerates missing fields but rejects non-objects",
          "[positionManager]") {
    const auto sparse = redis_positions::decodeDealReceipt("{}");
    REQUIRE(sparse.has_value());  // the caller substitutes "Unknown"
    CHECK(sparse->strategyId.empty());
    CHECK(sparse->strategyName.empty());

    CHECK_FALSE(redis_positions::decodeDealReceipt("not json").has_value());
    CHECK_FALSE(redis_positions::decodeDealReceipt("[]").has_value());
    CHECK_FALSE(redis_positions::decodeDealReceipt("null").has_value());
}
