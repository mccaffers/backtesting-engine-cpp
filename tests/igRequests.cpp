// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// IG wire-contract tests: the request bodies POST /positions/otc must carry
// (field names IG validates), the response parsing both engines rely on,
// the Version-header rule for the tunnelled DELETE, the shared Redis
// request-budget key formats, the gate policy matrix, and the open/close
// response mapping over an injected request seam — no Redis, no network.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "shared/aws/dynamoAuth.hpp"
#include "shared/ig/igRestClient.hpp"
#include "shared/redis/apiRequestGate.hpp"
#include "shared/redis/positionManager.hpp"

import igMarkets;
import igRequests;

namespace {

// A recording RequestFn: replies from a SCRIPT consumed one outcome per
// call (the last entry repeats — the confirm poll may retry), captures
// everything including the per-request options.
struct StubRequests {
    std::vector<ig::RequestOutcome> script;
    std::size_t calls = 0;
    std::vector<std::string> paths;
    std::vector<std::string> methods;
    std::vector<std::string> bodies;
    std::vector<ig_rest::Headers> headerSets;
    std::vector<ig::RequestOptions> options;
    std::vector<std::string> dealKeys;  // options[i].dealKey, for terse checks

    static ig::RequestOutcome responded(const long status, std::string body) {
        return ig::RequestOutcome{
            .fate = ig::RequestFate::Responded,
            .response = ig_rest::HttpResponse{status, std::move(body)}};
    }
    static ig::RequestOutcome refused(std::string reason = "duplicateDeal") {
        return ig::RequestOutcome{.fate = ig::RequestFate::Refused,
                                  .detail = std::move(reason)};
    }
    static ig::RequestOutcome transportFailed() {
        return ig::RequestOutcome{.fate = ig::RequestFate::TransportFailed,
                                  .detail = "transport failed"};
    }

    // Single fixed reply — the pre-confirm test idiom.
    void respond(ig::RequestOutcome outcome) {
        script = {std::move(outcome)};
    }

    ig::RequestFn fn() {
        return [this](const std::string& path, const std::string& method,
                      const std::string& jsonBody,
                      const ig_rest::Headers& extraHeaders,
                      const ig::RequestOptions& requestOptions)
                   -> ig::RequestOutcome {
            paths.push_back(path);
            methods.push_back(method);
            bodies.push_back(jsonBody);
            headerSets.push_back(extraHeaders);
            options.push_back(requestOptions);
            dealKeys.push_back(requestOptions.dealKey);
            if (script.empty()) {
                return transportFailed();
            }
            const std::size_t index = std::min(calls, script.size() - 1);
            ++calls;
            return script[index];
        };
    }
};

ig::TradeOpenObj makeOpenObj() {
    return ig::TradeOpenObj{
        .currencyCode = "USD",
        .epic = "TEST.EPIC.MINI.IP",
        .direction = "BUY",
        .size = 1.5,
        .stopDistance = 25,
        .limitDistance = 50,
        .dealReference = "ueur-L1719360000000",
    };
}

const ig::OrderContext kContext{
    .strategyUuid = "u-eur",
    .strategyName = "StubStrategy",
    .symbol = "EURUSD",
    .openDirection = "BUY",
};

}  // namespace

TEST_CASE("the open body carries the IG field names and values",
          "[igRequests]") {
    const auto body = nlohmann::json::parse(ig::encodeTradeOpen(makeOpenObj()));
    CHECK(body.at("currencyCode") == "USD");
    CHECK(body.at("epic") == "TEST.EPIC.MINI.IP");
    CHECK(body.at("expiry") == "-");
    CHECK(body.at("direction") == "BUY");
    CHECK(body.at("size") == 1.5);
    CHECK(body.at("forceOpen") == true);
    CHECK(body.at("guaranteedStop") == false);
    CHECK(body.at("orderType") == "MARKET");
    CHECK(body.at("stopDistance") == 25);
    CHECK(body.at("limitDistance") == 50);
    CHECK(body.at("dealReference") == "ueur-L1719360000000");
}

TEST_CASE("disarmed legs and an empty reference are omitted, not zeroed",
          "[igRequests]") {
    ig::TradeOpenObj order = makeOpenObj();
    order.stopDistance = 0;
    order.limitDistance = 0;
    order.dealReference.clear();
    const auto body = nlohmann::json::parse(ig::encodeTradeOpen(order));
    CHECK_FALSE(body.contains("stopDistance"));   // IG rejects a literal 0
    CHECK_FALSE(body.contains("limitDistance"));
    CHECK_FALSE(body.contains("dealReference"));  // empty fails IG's pattern
}

TEST_CASE("the close body is the C# TradeCloseObj shape", "[igRequests]") {
    const auto body = nlohmann::json::parse(ig::encodeTradeClose(
        ig::TradeCloseObj{.direction = "SELL", .dealId = "DEAL-77",
                          .size = 1.5}));
    CHECK(body.at("orderType") == "MARKET");
    CHECK(body.at("direction") == "SELL");
    CHECK(body.at("dealId") == "DEAL-77");
    CHECK(body.at("size") == 1.5);
}

TEST_CASE("the position response parser handles echo, error and garbage",
          "[igRequests]") {
    const auto ok = ig::parsePositionResponse(R"({"dealReference":"R1"})");
    REQUIRE(ok.has_value());
    CHECK(ok->dealReference == "R1");
    CHECK_FALSE(ok->errorCode.has_value());

    const auto error = ig::parsePositionResponse(
        R"({"dealReference":"","errorCode":"error.public-api.exceeded"})");
    REQUIRE(error.has_value());
    CHECK(error->dealReference.empty());
    CHECK(error->errorCode == "error.public-api.exceeded");

    // Null errorCode parses as absent (the C# nullable), and non-objects
    // are the "Failed to parse response" branch.
    const auto nullError = ig::parsePositionResponse(
        R"({"dealReference":"R2","errorCode":null})");
    REQUIRE(nullError.has_value());
    CHECK_FALSE(nullError->errorCode.has_value());
    CHECK_FALSE(ig::parsePositionResponse("<html>gateway</html>").has_value());
    CHECK_FALSE(ig::parsePositionResponse("[1,2]").has_value());
    CHECK_FALSE(ig::parsePositionResponse("").has_value());
}

TEST_CASE("closing direction flips the open side", "[igRequests]") {
    CHECK(ig::closingDirection("BUY") == "SELL");
    CHECK(ig::closingDirection("SELL") == "BUY");
}

TEST_CASE("the Version header is 1 only for the tunnelled DELETE",
          "[igRequests]") {
    CHECK(ig_rest::versionFor({}) == "2");
    CHECK(ig_rest::versionFor({{"_method", "DELETE"}}) == "1");
    CHECK(ig_rest::versionFor({{"_method", "delete"}}) == "1");  // C# ignores case
    CHECK(ig_rest::versionFor({{"_method", "PATCH"}}) == "2");
    CHECK(ig_rest::versionFor({{"X-Other", "DELETE"}}) == "2");
}

TEST_CASE("the request budget keys match the C# IGMarketRequests formats",
          "[igRequests]") {
    CHECK(redis_api::requestWindowKey(7) == "REQ#7");
    CHECK(redis_api::requestWindowKey(59) == "REQ#59");
    CHECK(redis_api::dealRequestKey("u-eurBUY") == "API#u-eurBUY");
}

TEST_CASE("the gate policy fails closed for opens and open for closes",
          "[igRequests]") {
    using ig::gatePolicy;
    using ig::GateRefusal;
    constexpr bool kOpen = false;   // riskReducing
    constexpr bool kClose = true;

    // A definite duplicate refuses BOTH classes (for a close this is pacing
    // on its own close#<dealId> marker — safe now that a refused close maps
    // to Failed and keeps the book entry).
    CHECK(gatePolicy(true, 0, kOpen) == GateRefusal::DuplicateDeal);
    CHECK(gatePolicy(true, 0, kClose) == GateRefusal::DuplicateDeal);

    // Nothing suspicious: proceed.
    CHECK_FALSE(gatePolicy(false, 0, kOpen).has_value());
    CHECK_FALSE(gatePolicy(false, 0, kClose).has_value());

    // Redis uncertainty: opens fail closed, closes fail OPEN — an unsent
    // close leaves live exposure, the one outcome worse than a duplicate.
    CHECK(gatePolicy(std::nullopt, 0, kOpen) == GateRefusal::DuplicateUnknown);
    CHECK_FALSE(gatePolicy(std::nullopt, std::nullopt, kClose).has_value());
    CHECK(gatePolicy(false, std::nullopt, kOpen) == GateRefusal::BudgetUnknown);
    CHECK_FALSE(gatePolicy(false, std::nullopt, kClose).has_value());

    // Soft budget exhausted (known): opens brake, closes bypass — IG's hard
    // limiter is the backstop and a rate-rejected close retries via sync.
    const int over = ig::IGMarketRequests::kMaxRequestsPerMinute + 1;
    CHECK(gatePolicy(false, over, kOpen) == GateRefusal::RateLimited);
    CHECK_FALSE(gatePolicy(false, over, kClose).has_value());
}

TEST_CASE("makeOpen confirms the deal and resolves the dealId",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::responded(200, R"({"dealReference":"IGREF-1"})"),
        StubRequests::responded(
            200,
            R"({"dealId":"DIAAA-1","dealReference":"IGREF-1",)"
            R"("dealStatus":"ACCEPTED","reason":"SUCCESS"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Accepted);
    CHECK(result.dealReference == "IGREF-1");
    CHECK(result.dealId == "DIAAA-1");

    // POST to the OTC endpoint (deduplicated per strategy+direction, the C#
    // $"{strategyId}{direction}"), then the confirm GET: Version 1, no
    // body, and an EMPTY deal key — the POST just recorded API#u-eurBUY for
    // 30s, so reusing it would refuse this very confirm as a duplicate.
    REQUIRE(stub.paths.size() == 2);
    CHECK(stub.paths[0] == "/positions/otc");
    CHECK(stub.methods[0] == "POST");
    CHECK(stub.dealKeys[0] == "u-eurBUY");
    CHECK(stub.headerSets[0].empty());
    CHECK(nlohmann::json::parse(stub.bodies[0]).at("epic")
          == "TEST.EPIC.MINI.IP");
    CHECK(stub.paths[1] == "/confirms/IGREF-1");
    CHECK(stub.methods[1] == "GET");
    CHECK(stub.bodies[1].empty());
    CHECK(stub.headerSets[1]
          == ig_rest::Headers{{"Version", "1"}});
    CHECK(stub.dealKeys[1].empty());

    // The open POST is NOT idempotent: zero transport retries (ambiguity
    // resolves via /confirms, never a blind re-send), fail-closed gating.
    // The confirm GET is idempotent and keeps the retried default.
    CHECK(stub.options[0].transportRetries == 0);
    CHECK_FALSE(stub.options[0].riskReducing);
    CHECK(stub.options[1].transportRetries == 2);
    CHECK_FALSE(stub.options[1].riskReducing);
}

TEST_CASE("a REJECTED confirm maps to Rejected with the broker's reason",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::responded(200, R"({"dealReference":"IGREF-1"})"),
        StubRequests::responded(
            200, R"({"dealStatus":"REJECTED","reason":"INSUFFICIENT_FUNDS"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Rejected);
    CHECK(result.reason == "INSUFFICIENT_FUNDS");
    CHECK(stub.paths.size() == 2);  // definitive answer — no further polling
}

TEST_CASE("confirm 404s exhaust the retries and keep Accepted with an empty "
          "dealId",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::responded(200, R"({"dealReference":"IGREF-1"})"),
        StubRequests::responded(404, R"({"errorCode":"not found"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    // The POST succeeded, so IG has the order: mapping to Rejected would
    // release the lock and skip booking a possibly-live deal. Accepted
    // with an empty dealId books it and fails closed at close time.
    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Accepted);
    CHECK(result.dealReference == "IGREF-1");
    CHECK(result.dealId.empty());
    CHECK(stub.paths.size() == 4);  // 1 POST + 3 confirm attempts
}

TEST_CASE("an unparseable confirm keeps Accepted with an empty dealId",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::responded(200, R"({"dealReference":"IGREF-1"})"),
        StubRequests::responded(200, "<html>proxy</html>"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 2, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Accepted);
    CHECK(result.dealId.empty());
    CHECK(stub.paths.size() == 3);  // 1 POST + 2 confirm attempts
}

TEST_CASE("a confirm poll uses IG's echoed reference, falling back to ours",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::responded(200, R"({"dealReference":""})"),  // no echo
        StubRequests::responded(
            200, R"({"dealId":"DIAAA-2","dealStatus":"ACCEPTED"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Accepted);
    CHECK(result.dealId == "DIAAA-2");
    REQUIRE(stub.paths.size() == 2);
    // Our minted reference names the deal when IG echoes nothing.
    CHECK(stub.paths[1] == "/confirms/ueur-L1719360000000");
}

TEST_CASE("a transport-failed open resolves through the confirms poll, "
          "never a re-send",
          "[igRequests]") {
    // The lost-ACK case: the POST may or may not have reached IG. A blind
    // retry could double a live position — the ONLY acceptable resolution
    // is asking /confirms under the dealReference we minted into the body.
    StubRequests stub;
    stub.script = {
        StubRequests::transportFailed(),
        StubRequests::responded(
            200, R"({"dealId":"DIAAA-9","dealStatus":"ACCEPTED"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Accepted);
    CHECK(result.dealReference == "ueur-L1719360000000");
    CHECK(result.dealId == "DIAAA-9");

    // Exactly ONE POST — the order body was never re-sent.
    REQUIRE(stub.paths.size() == 2);
    CHECK(stub.methods[0] == "POST");
    CHECK(stub.paths[1] == "/confirms/ueur-L1719360000000");
    CHECK(stub.methods[1] == "GET");
}

TEST_CASE("a transport-failed open whose confirm says REJECTED maps to "
          "Rejected",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::transportFailed(),
        StubRequests::responded(
            200, R"({"dealStatus":"REJECTED","reason":"MARKET_CLOSED"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Rejected);
    CHECK(result.reason == "MARKET_CLOSED");
    CHECK(stub.paths.size() == 2);
}

TEST_CASE("an unconfirmable transport-failed open is Failed with exactly "
          "one POST",
          "[igRequests]") {
    StubRequests stub;
    stub.script = {
        StubRequests::transportFailed(),
        StubRequests::responded(404, R"({"errorCode":"not found"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    // No confirm ever appeared: the order presumably never reached IG. It
    // is NOT re-sent (the channel's failure TTL brakes re-entry, and the
    // producer's book sync seeds the position if it DID land).
    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Failed);
    CHECK(result.reason.contains("not re-sent"));
    const auto posts = std::count(stub.methods.begin(), stub.methods.end(),
                                  std::string{"POST"});
    CHECK(posts == 1);
    CHECK(stub.paths.size() == 4);  // 1 POST + 3 confirm attempts
}

TEST_CASE("a 502 on the open POST is ambiguous and resolves like a lost ACK",
          "[igRequests]") {
    // A gateway 5xx does not say whether IG holds the order — same
    // confirm-driven resolution, same single-POST guarantee.
    StubRequests stub;
    stub.script = {
        StubRequests::responded(502, "<html>bad gateway</html>"),
        StubRequests::responded(
            200, R"({"dealId":"DIAAA-5","dealStatus":"ACCEPTED"})"),
    };
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    const ig::OpenResult result = open(makeOpenObj(), kContext);
    CHECK(result.status == ig::OpenStatus::Accepted);
    CHECK(result.dealId == "DIAAA-5");
    const auto posts = std::count(stub.methods.begin(), stub.methods.end(),
                                  std::string{"POST"});
    CHECK(posts == 1);
}

TEST_CASE("makeOpen fails on refusal, non-200, garbage and errorCode",
          "[igRequests]") {
    StubRequests stub;
    const auto open =
        ig::IGMarketCalls::makeOpen(stub.fn(), 3, std::chrono::milliseconds{0});

    stub.respond(StubRequests::refused("budgetUnknown"));  // gate refused
    const auto refused = open(makeOpenObj(), kContext);
    CHECK(refused.status == ig::OpenStatus::Failed);
    CHECK(refused.reason.contains("budgetUnknown"));

    stub.respond(StubRequests::responded(401, R"({"errorCode":"invalid"})"));
    const auto denied = open(makeOpenObj(), kContext);
    CHECK(denied.status == ig::OpenStatus::Failed);
    CHECK(denied.reason.contains("401"));

    stub.respond(StubRequests::responded(200, "<html>proxy error</html>"));
    CHECK(open(makeOpenObj(), kContext).status == ig::OpenStatus::Failed);

    stub.respond(StubRequests::responded(
        200, R"({"dealReference":"","errorCode":"error.margin"})"));
    const auto margin = open(makeOpenObj(), kContext);
    CHECK(margin.status == ig::OpenStatus::Failed);
    CHECK(margin.reason.contains("error.margin"));
}

TEST_CASE("parseDealConfirmation handles the IG shape, partial fields and "
          "garbage",
          "[igRequests]") {
    const auto full = ig::parseDealConfirmation(
        R"({"dealId":"DIAAA-3","dealReference":"IGREF-7",)"
        R"("dealStatus":"ACCEPTED","reason":"SUCCESS"})");
    REQUIRE(full.has_value());
    CHECK(full->dealId == "DIAAA-3");
    CHECK(full->dealReference == "IGREF-7");
    CHECK(full->dealStatus == "ACCEPTED");
    CHECK(full->reason == "SUCCESS");

    // Missing fields parse as empty (empty dealStatus = still pending).
    const auto partial = ig::parseDealConfirmation(R"({"dealId":"D"})");
    REQUIRE(partial.has_value());
    CHECK(partial->dealStatus.empty());

    CHECK_FALSE(ig::parseDealConfirmation("<html>oops</html>").has_value());
    CHECK_FALSE(ig::parseDealConfirmation("[]").has_value());
    CHECK_FALSE(ig::parseDealConfirmation("").has_value());
}

TEST_CASE("an explicit Version header suppresses the default",
          "[igRequests]") {
    CHECK(ig_rest::hasHeader({{"Version", "1"}}, "Version"));
    CHECK(ig_rest::hasHeader({{"version", "1"}}, "Version"));  // case-insensitive
    CHECK(ig_rest::hasHeader({{"_method", "DELETE"}, {"Version", "1"}},
                             "version"));
    CHECK_FALSE(ig_rest::hasHeader({}, "Version"));
    CHECK_FALSE(ig_rest::hasHeader({{"_method", "DELETE"}}, "Version"));
}

TEST_CASE("makeClose tunnels DELETE and maps outcomes", "[igRequests]") {
    StubRequests stub;
    const auto close = ig::IGMarketCalls::makeClose(stub.fn());
    const ig::TradeCloseObj closeObj{.direction = "SELL", .dealId = "DEAL-77",
                                     .size = 1.5};

    stub.respond(StubRequests::responded(
        200, R"({"dealReference":"IGREF-C"})"));
    CHECK(close(closeObj, kContext).status == ig::CloseStatus::Ok);
    REQUIRE(stub.headerSets.size() == 1);
    REQUIRE(stub.headerSets[0].size() == 1);
    CHECK(stub.headerSets[0][0]
          == std::pair<std::string, std::string>{"_method", "DELETE"});
    CHECK(stub.paths[0] == "/positions/otc");
    CHECK(stub.methods[0] == "POST");
    CHECK(nlohmann::json::parse(stub.bodies[0]).at("dealId") == "DEAL-77");

    // The C# null-response branch, now reserved for a REAL lost exchange.
    stub.respond(StubRequests::transportFailed());
    CHECK(close(closeObj, kContext).status == ig::CloseStatus::Gone);

    stub.respond(StubRequests::responded(500, "oops"));
    CHECK(close(closeObj, kContext).status == ig::CloseStatus::Failed);

    stub.respond(StubRequests::responded(200, "not json"));
    CHECK(close(closeObj, kContext).status == ig::CloseStatus::Failed);
}

TEST_CASE("a close is keyed on its dealId, never the open's uuid+direction",
          "[igRequests]") {
    // The L1 regression: open and close used to SHARE API#<uuid><BUY|SELL>,
    // so the open's 30s marker refused the close that followed it — and the
    // refusal read as "position missing from IG", pruning a live position
    // from the book. Distinct namespaces make that collision impossible.
    StubRequests stub;
    stub.respond(StubRequests::responded(
        200, R"({"dealReference":"IGREF-C"})"));
    const auto close = ig::IGMarketCalls::makeClose(stub.fn());

    close(ig::TradeCloseObj{.direction = "SELL", .dealId = "DEAL-77",
                            .size = 1.5},
          kContext);
    REQUIRE(stub.dealKeys.size() == 1);
    CHECK(stub.dealKeys[0] == "close#DEAL-77");
    CHECK(stub.dealKeys[0] != kContext.strategyUuid + kContext.openDirection);

    // And the close is risk-reducing with transport retries kept on
    // (re-sending a close of the same dealId is safe, unlike the open).
    CHECK(stub.options[0].riskReducing);
    CHECK(stub.options[0].transportRetries == 2);
}

TEST_CASE("a refused close maps to Failed and never to Gone", "[igRequests]") {
    // The L2 regression: a refusal means the request was NEVER SENT, so
    // "the position is missing from IG" is unknowable — Gone deleted the
    // very book entry the sync-driven close retry depends on. Failed keeps
    // it, and the strategy re-closes on the next book sync.
    StubRequests stub;
    stub.respond(StubRequests::refused("duplicateDeal"));
    const auto close = ig::IGMarketCalls::makeClose(stub.fn());

    const ig::CloseResult result =
        close(ig::TradeCloseObj{.direction = "SELL", .dealId = "DEAL-77",
                                .size = 1.5},
              kContext);
    CHECK(result.status == ig::CloseStatus::Failed);
    CHECK(result.reason.contains("duplicateDeal"));
}

TEST_CASE("the deal receipt key matches the C# format", "[igRequests]") {
    // (Declared in positionManager but pinned here with the rest of the IG
    // wire contract.)
    CHECK(redis_positions::dealReceiptKey("IGREF-9", "EURUSD")
          == "DealId#IGREF-9#EURUSD");
}

TEST_CASE("the DynamoDB auth key matches the C# Auth# convention",
          "[igRequests]") {
    CHECK(aws_auth::authKey("live") == "Auth#live");
    CHECK(aws_auth::authKey("demo") == "Auth#demo");
    CHECK(aws_auth::kAuthTable == "MarketDataLive");
}

TEST_CASE("a DynamoDB session item maps to an IG auth only when complete",
          "[igRequests]") {
    const std::map<std::string, std::string> full{
        {"id", "Auth#demo"},
        {"sort", "null"},
        {"url", "https://demo-api.ig.com/gateway/deal"},
        {"apikey", "key-1"},
        {"CST", "cst-1"},
        {"xSecurityToken", "xst-1"},
        {"date", "2026-07-05T00:00:00Z"},  // ignored, like authRoot
    };
    const auto auth = aws_auth::authFromItem(full);
    REQUIRE(auth.has_value());
    CHECK(auth->url == "https://demo-api.ig.com/gateway/deal");
    CHECK(auth->apiKey == "key-1");
    CHECK(auth->cst == "cst-1");
    CHECK(auth->xSecurityToken == "xst-1");

    // Any missing or empty session field is unusable — must read as no auth.
    for (const char* required : {"url", "apikey", "CST", "xSecurityToken"}) {
        auto incomplete = full;
        incomplete.erase(required);
        CHECK_FALSE(aws_auth::authFromItem(incomplete).has_value());
        auto blank = full;
        blank[required] = "";
        CHECK_FALSE(aws_auth::authFromItem(blank).has_value());
    }
    CHECK_FALSE(aws_auth::authFromItem({}).has_value());
}
