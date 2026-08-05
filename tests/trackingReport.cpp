// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// trackingReport — the pure half of the tracking consumer: the PH#-then-PO#
// lookup order, the close-pip arithmetic (scales INJECTED — the machinery is
// under test, not the current symbolScale table), and the live-trades
// document shape. No Redis, no Elastic.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

#include "shared/redis/positionManager.hpp"

import dealPacket;
import trackingReport;

using Catch::Approx;

namespace {

// A decodable PO#/PH# payload via the real codec, so these tests track the
// stored wire shape without hand-rolling JSON.
std::string samplePayload() {
    redis_positions::PositionRecord record;
    record.dealId = "DIAAAA";
    record.dealReference = "ref-1";
    record.symbol = "EURUSD";
    record.epic = "CS.D.EURUSD.MINI.IP";
    record.direction = "BUY";
    record.size = 1.5;
    record.level = 117000;
    record.strategyId = "strat-1";
    record.strategyName = "TestStrategy";
    record.openedAtMicros = 1234567890;
    return redis_positions::encodePositionRecord(record);
}

deal_packet::Deal sampleDeal() {
    deal_packet::Deal deal;
    deal.level = 1.17123;
    deal.size = 1.5;
    deal.stopLevel = std::nullopt;
    deal.limitLevel = std::nullopt;
    deal.dealReference = "ref-1";
    deal.dealId = "DIAAAA";
    deal.dealIdOrigin = "DIAAAA";
    deal.epic = "CS.D.EURUSD.MINI.IP";
    deal.direction = "SELL";  // the closing side
    deal.status = "DELETED";
    deal.dealStatus = "ACCEPTED";
    deal.currency = "GBP";
    deal.channel = "WTP";
    deal.expiry = "-";
    deal.timestamp = "2026-07-14T09:30:00.250";
    deal.guaranteedStop = "false";
    return deal;
}

// Getter stubs that record the call order.
tracking_report::PayloadGetter getterOf(
    std::vector<std::string>& calls, const std::string& name,
    std::optional<std::optional<std::string>> result) {
    return [&calls, name, result = std::move(result)](const std::string&) {
        calls.push_back(name);
        return result;
    };
}

}  // namespace

// ---- computeClosePips -------------------------------------------------------

TEST_CASE("computeClosePips converts an FX-style close to pips",
          "[trackingReport]") {
    // 1.17123 x 100000 = 117123 points; (117123 - 117000) / 10 = 12.3 pips.
    const auto pips =
        tracking_report::computeClosePips(1.17123, 117000, "BUY", 1.0,
                                          100000, 10);
    REQUIRE(pips.has_value());
    CHECK(*pips == Approx(12.3));
}

TEST_CASE("computeClosePips weights by position size", "[trackingReport]") {
    const auto pips =
        tracking_report::computeClosePips(1.17123, 117000, "BUY", 2.5,
                                          100000, 10);
    REQUIRE(pips.has_value());
    CHECK(*pips == Approx(30.75));
}

TEST_CASE("computeClosePips signs by the position direction",
          "[trackingReport]") {
    // Same price move: a gain for the BUY is a loss for the SELL, and a BUY
    // closing below its open goes negative.
    const auto sell =
        tracking_report::computeClosePips(1.17123, 117000, "SELL", 1.0,
                                          100000, 10);
    REQUIRE(sell.has_value());
    CHECK(*sell == Approx(-12.3));

    const auto losingBuy =
        tracking_report::computeClosePips(1.16900, 117000, "BUY", 1.0,
                                          100000, 10);
    REQUIRE(losingBuy.has_value());
    CHECK(*losingBuy == Approx(-10.0));
}

TEST_CASE("computeClosePips handles a JPY-style scale pair",
          "[trackingReport]") {
    // priceScale 1000, 10 points per pip: 154.750 closes a 154500-point open
    // 250 points = 25 pips higher.
    const auto pips =
        tracking_report::computeClosePips(154.750, 154500, "BUY", 1.0,
                                          1000, 10);
    REQUIRE(pips.has_value());
    CHECK(*pips == Approx(25.0));
}

TEST_CASE("computeClosePips handles a metals-style scale pair",
          "[trackingReport]") {
    // priceScale 1000, 1000 points per pip: exactly one pip.
    const auto pips =
        tracking_report::computeClosePips(2346.678, 2345678, "BUY", 1.0,
                                          1000, 1000);
    REQUIRE(pips.has_value());
    CHECK(*pips == Approx(1.0));
}

TEST_CASE("computeClosePips refuses unknown scales", "[trackingReport]") {
    // symbol_scale::kUnknown is 0 — either scale missing must skip the calc,
    // not multiply/divide the P&L by zero.
    CHECK_FALSE(tracking_report::computeClosePips(1.17123, 117000, "BUY", 1.0,
                                                  0, 10)
                    .has_value());
    CHECK_FALSE(tracking_report::computeClosePips(1.17123, 117000, "BUY", 1.0,
                                                  100000, 0)
                    .has_value());
}

// ---- lookupPosition ---------------------------------------------------------

TEST_CASE("lookupPosition reads history first and skips the live getter on a "
          "hit",
          "[trackingReport]") {
    std::vector<std::string> calls;
    const auto record = tracking_report::lookupPosition(
        "ref-1", getterOf(calls, "PH", std::optional(samplePayload())),
        getterOf(calls, "PO", std::optional(samplePayload())));
    REQUIRE(record.has_value());
    CHECK(record->strategyId == "strat-1");
    CHECK(calls == std::vector<std::string>{"PH"});
}

TEST_CASE("lookupPosition falls back to the live key when history misses",
          "[trackingReport]") {
    std::vector<std::string> calls;
    // Outer engaged, inner empty: the read worked, the key is missing.
    const auto missing =
        std::optional<std::optional<std::string>>{std::optional<std::string>{}};
    const auto record = tracking_report::lookupPosition(
        "ref-1", getterOf(calls, "PH", missing),
        getterOf(calls, "PO", std::optional(samplePayload())));
    REQUIRE(record.has_value());
    CHECK(record->symbol == "EURUSD");
    CHECK(calls == std::vector<std::string>{"PH", "PO"});
}

TEST_CASE("lookupPosition falls back when the history read fails",
          "[trackingReport]") {
    std::vector<std::string> calls;
    const auto record = tracking_report::lookupPosition(
        "ref-1", getterOf(calls, "PH", std::nullopt),  // outer: Redis failure
        getterOf(calls, "PO", std::optional(samplePayload())));
    REQUIRE(record.has_value());
    CHECK(calls == std::vector<std::string>{"PH", "PO"});
}

TEST_CASE("lookupPosition falls back past an undecodable history payload",
          "[trackingReport]") {
    std::vector<std::string> calls;
    const auto record = tracking_report::lookupPosition(
        "ref-1", getterOf(calls, "PH", std::optional(std::string{"not json"})),
        getterOf(calls, "PO", std::optional(samplePayload())));
    REQUIRE(record.has_value());
    CHECK(calls == std::vector<std::string>{"PH", "PO"});
}

TEST_CASE("lookupPosition yields nothing when both families miss",
          "[trackingReport]") {
    std::vector<std::string> calls;
    const auto missing =
        std::optional<std::optional<std::string>>{std::optional<std::string>{}};
    const auto record = tracking_report::lookupPosition(
        "ref-1", getterOf(calls, "PH", missing), getterOf(calls, "PO", missing));
    CHECK_FALSE(record.has_value());
    CHECK(calls == std::vector<std::string>{"PH", "PO"});
}

// ---- serializeDeal ----------------------------------------------------------

TEST_CASE("serializeDeal carries every field, absent optionals as null",
          "[trackingReport]") {
    const auto deal = sampleDeal();
    const auto parsed = nlohmann::json::parse(tracking_report::serializeDeal(deal));
    CHECK(parsed.size() == 16);
    CHECK(parsed["level"].get<double>() == Approx(1.17123));
    CHECK(parsed["size"].get<double>() == Approx(1.5));
    CHECK(parsed["stopLevel"].is_null());
    CHECK(parsed["limitLevel"].is_null());
    CHECK(parsed["dealReference"] == "ref-1");
    CHECK(parsed["dealId"] == "DIAAAA");
    CHECK(parsed["dealIdOrigin"] == "DIAAAA");
    CHECK(parsed["epic"] == "CS.D.EURUSD.MINI.IP");
    CHECK(parsed["direction"] == "SELL");
    CHECK(parsed["status"] == "DELETED");
    CHECK(parsed["dealStatus"] == "ACCEPTED");
    CHECK(parsed["currency"] == "GBP");
    CHECK(parsed["channel"] == "WTP");
    CHECK(parsed["expiry"] == "-");
    CHECK(parsed["timestamp"] == "2026-07-14T09:30:00.250");
    CHECK(parsed["guaranteedStop"] == "false");
}

// ---- buildLiveTradeDocument -------------------------------------------------

TEST_CASE("buildLiveTradeDocument enriches from the position record",
          "[trackingReport]") {
    const auto deal = sampleDeal();
    const auto record =
        redis_positions::decodePositionRecord(samplePayload());
    REQUIRE(record.has_value());

    const auto parsed = nlohmann::json::parse(
        tracking_report::buildLiveTradeDocument(deal, record, "FALLBACK",
                                                "demo",
                                                "2026-07-14T09:30:01Z", 12.3));
    CHECK(parsed["date"] == "2026-07-14T09:30:01Z");
    CHECK(parsed["env"] == "demo");
    CHECK(parsed["symbol"] == "EURUSD");  // the position's, not the fallback
    CHECK(parsed["action"] == "DELETED");
    CHECK(parsed["strategy"] == "strat-1");
    CHECK(parsed["dealId"] == "DIAAAA");
    CHECK(parsed["dealReference"] == "ref-1");
    CHECK(parsed["level"].get<double>() == Approx(1.17123));
    CHECK(parsed["pips"].get<double>() == Approx(12.3));
    // The raw-deal audit string is itself valid JSON of the same deal.
    const auto inner = nlohmann::json::parse(parsed["json"].get<std::string>());
    CHECK(inner["dealReference"] == "ref-1");
    CHECK(inner["stopLevel"].is_null());
}

TEST_CASE("buildLiveTradeDocument degrades to fallbacks without a record",
          "[trackingReport]") {
    auto deal = sampleDeal();
    deal.dealId.clear();
    deal.level = std::nullopt;

    const auto parsed = nlohmann::json::parse(
        tracking_report::buildLiveTradeDocument(deal, std::nullopt, "EURUSD",
                                                "demo", "2026-07-14T09:30:01Z",
                                                std::nullopt));
    CHECK(parsed["symbol"] == "EURUSD");  // the epic-mapped fallback
    CHECK(parsed["strategy"] == "Unknown");
    CHECK_FALSE(parsed.contains("dealId"));  // omitted when empty
    CHECK_FALSE(parsed.contains("level"));   // omitted when absent
    CHECK_FALSE(parsed.contains("pips"));
}

TEST_CASE("buildLiveTradeDocument treats a zero level as absent",
          "[trackingReport]") {
    auto deal = sampleDeal();
    deal.level = 0.0;  // IG sends 0 on some UPDATED events
    const auto parsed = nlohmann::json::parse(
        tracking_report::buildLiveTradeDocument(deal, std::nullopt, "EURUSD",
                                                "demo", "2026-07-14T09:30:01Z",
                                                std::nullopt));
    CHECK_FALSE(parsed.contains("level"));
}
