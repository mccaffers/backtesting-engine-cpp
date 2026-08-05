// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "run/reporting/tradeDocument.hpp"

namespace {

// A fully-populated document (armed stop and limit) for serialisation checks.
TradeDocument sampleDoc() {
    return TradeDocument{
        .RUN_ID = "run-42",
        .timestamp = "2024-03-01T10:15:30.250Z",
        .ingestedAt = "2026-07-04T12:00:00Z",
        .hostname = "test-host",
        .strategyUuid = "uuid-1",
        .strategyName = "RandomStrategy",
        .tradeId = "T7",
        .symbol = "EURUSD",
        .direction = "LONG",
        .size = 2,
        .entryPrice = 1.10001,
        .entryBid = 1.09999,
        .entryAsk = 1.10001,
        .closePrice = 1.10051,
        .stopPrice = 1.09899,
        .limitPrice = 1.10099,
        .stopDistancePips = 10,
        .limitDistancePips = 10,
        .openTime = "2024-03-01T10:00:00.000Z",
        .closeTime = "2024-03-01T10:15:30.250Z",
        .holdSeconds = 930.25,
        .pnlPoints = 100,
        .pnlPips = 5.0,
        .liquidated = false,
        .scalingFactor = 10,
        .priceScale = 100000,
    };
}

}  // namespace

TEST_CASE("TradeDocument serialises the Kibana wire shape", "[tradeDocument]") {
    const nlohmann::json j = sampleDoc();

    // The timestamp field must land as `@timestamp` (the Kibana convention
    // shared with the per-run docs), not as a literal "timestamp" key.
    CHECK(j.at("@timestamp") == "2024-03-01T10:15:30.250Z");
    CHECK_FALSE(j.contains("timestamp"));

    CHECK(j.at("RUN_ID") == "run-42");
    CHECK(j.at("ingestedAt") == "2026-07-04T12:00:00Z");
    CHECK(j.at("hostname") == "test-host");
    CHECK(j.at("strategyUuid") == "uuid-1");
    CHECK(j.at("strategyName") == "RandomStrategy");
    CHECK(j.at("tradeId") == "T7");
    CHECK(j.at("symbol") == "EURUSD");
    CHECK(j.at("direction") == "LONG");
    CHECK(j.at("size") == 2);
    CHECK(j.at("entryPrice") == 1.10001);
    CHECK(j.at("closePrice") == 1.10051);
    CHECK(j.at("openTime") == "2024-03-01T10:00:00.000Z");
    CHECK(j.at("closeTime") == "2024-03-01T10:15:30.250Z");
    CHECK(j.at("holdSeconds") == 930.25);
    CHECK(j.at("pnlPoints") == 100);
    CHECK(j.at("pnlPips") == 5.0);
    CHECK(j.at("liquidated") == false);
    CHECK(j.at("scalingFactor") == 10);
    CHECK(j.at("priceScale") == 100000);

    // Both exit legs are armed here, so real levels must be emitted.
    CHECK(j.at("stopPrice") == 1.09899);
    CHECK(j.at("limitPrice") == 1.10099);
}

TEST_CASE("TradeDocument nulls disarmed stop/limit levels", "[tradeDocument]") {
    // A zero distance means the leg was never armed: the precomputed trigger
    // price is meaningless and must serialise as null, not as a fake level.
    TradeDocument doc = sampleDoc();
    doc.stopDistancePips = 0;
    doc.limitDistancePips = 0;

    const nlohmann::json j = doc;
    CHECK(j.at("stopPrice").is_null());
    CHECK(j.at("limitPrice").is_null());
}

TEST_CASE("isoUtcMillis formats UTC with millisecond precision",
          "[tradeDocument]") {
    using namespace std::chrono;

    // 2024-03-01T10:15:30.250Z, built from a known epoch offset.
    const auto tp = std::chrono::sys_days{2024y / 3 / 1} + 10h + 15min + 30s + 250ms;
    CHECK(TradeDocument::isoUtcMillis(tp) == "2024-03-01T10:15:30.250Z");

    // Whole seconds keep the .000 so the field parses as a consistent date
    // format in Elasticsearch.
    const auto whole = std::chrono::sys_days{2024y / 3 / 1};
    CHECK(TradeDocument::isoUtcMillis(whole) == "2024-03-01T00:00:00.000Z");

    // Ticks are sub-second; two closes 1ms apart must not collapse onto the
    // same stamp.
    CHECK(TradeDocument::isoUtcMillis(tp + 1ms) == "2024-03-01T10:15:30.251Z");
}
