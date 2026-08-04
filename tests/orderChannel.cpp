// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// OrderChannel flow tests: the C# Request() semantics around one placement
// (market lookup, size modifier, in-flight lock extension, and the outcome
// bookkeeping — Accepted -> book under IG's dealReference, Rejected ->
// release, Failed -> the 2-minute brake) plus the ClosePosition flow
// (direction flip, missing-dealId guard, the recently-closed window, and
// the remove-on-gone branch). Broker and Redis are injected seams
// (PlaceOrder/PlaceClose, Hooks, MarketLookup), so no server is involved,
// and the market table itself is stubbed — these tests pin the machinery,
// not the currently-listed markets.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import igMarkets;
import liveStrategyRunner;
import marketDefinitions;
import orderChannel;
import orderRequest;
import symbolScale;
import trade;

namespace {

constexpr std::chrono::system_clock::time_point kTick{
    std::chrono::microseconds{1'719'360'000'000'000LL}};

// The mini epic is what must reach the broker; the CFD epic is a decoy the
// channel must NOT pick up. tradeSizeModifier 0.5 exercises the C#
// TradeSizeModifier path.
constexpr live::MarketDefinition kEurUsd{
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

const live::MarketLookup kTestLookup =
    [](std::string_view symbol) -> const live::MarketDefinition* {
    return symbol == "EURUSD" ? &kEurUsd : nullptr;
};

// Records every hook invocation, in order, with the arguments the channel
// passed. All hooks report success unless the test flips the flags.
struct HookRecorder {
    struct Extend {
        std::string uuid;
        std::string direction;
        std::chrono::seconds ttl;
    };
    std::vector<std::string> sequence;  // "cluster"/"extend"/"release"/...
    std::vector<Extend> extends;
    std::vector<std::pair<std::string, std::string>> releases;  // uuid, dir
    std::vector<std::pair<std::string, std::string>> saves;   // ref, payload
    std::vector<std::pair<std::string, std::string>> adds;    // uuid, ref
    std::vector<std::pair<std::string, std::string>> receipts;  // ref, payload
    std::vector<std::pair<std::string, std::string>> removes;   // uuid, ref
    std::vector<std::pair<std::string, std::string>> clusterChecks;  // sym, strat
    bool saveOk = true;
    bool clusterAllow = true;

    live::OrderChannel::Hooks hooks() {
        return live::OrderChannel::Hooks{
            .clusterBlocked = [this](const std::string& symbol,
                                     const std::string& strategyName) {
                sequence.emplace_back("cluster");
                clusterChecks.emplace_back(symbol, strategyName);
                return !clusterAllow;
            },
            .extendLock = [this](const std::string& uuid,
                                 const std::string& direction,
                                 const std::chrono::seconds ttl) {
                sequence.emplace_back("extend");
                extends.push_back({uuid, direction, ttl});
                return true;
            },
            .releaseLock = [this](const std::string& uuid,
                                  const std::string& direction) {
                sequence.emplace_back("release");
                releases.emplace_back(uuid, direction);
                return true;
            },
            .savePosition = [this](const std::string& reference,
                                   const std::string& payload) {
                sequence.emplace_back("save");
                saves.emplace_back(reference, payload);
                return saveOk;
            },
            .addPosition = [this](const std::string& uuid,
                                  const std::string& reference) {
                sequence.emplace_back("add");
                adds.emplace_back(uuid, reference);
                return true;
            },
            .saveDealReceipt = [this](const std::string& reference,
                                      const std::string& symbol,
                                      const std::string& payload) {
                sequence.emplace_back("receipt");
                receipts.emplace_back(reference + "#" + symbol, payload);
                return true;
            },
            .removePosition = [this](const std::string& uuid,
                                     const std::string& reference) {
                sequence.emplace_back("remove");
                removes.emplace_back(uuid, reference);
                return true;
            },
        };
    }
};

live::OrderRequest makeRequest(const Direction direction = Direction::LONG,
                               const std::string& symbol = "EURUSD") {
    const auto request = live::makeOrderRequest(live::OrderIntent{
        .strategyName = "StubStrategy",
        .strategyUuid = "u-eur",
        .symbol = "EURUSD",  // must exist in symbolScale to build
        .direction = direction,
        .size = 3,
        .stopDistancePips = 25,
        .limitDistancePips = 50,
        .bid = 110000,
        .ask = 110002,
        .timestamp = kTick,
    });
    REQUIRE(request.has_value());
    live::OrderRequest result = *request;
    result.symbol = symbol;  // fake-symbol tests exercise the market lookup
    return result;
}

live::CloseRequest makeCloseRequest(const std::string& dealId = "DEAL-77") {
    return live::CloseRequest{
        .strategyUuid = "u-eur",
        .strategyName = "StubStrategy",
        .symbol = "EURUSD",
        .direction = Direction::LONG,  // open position was LONG (broker BUY)
        .size = 1.5,
        .dealId = dealId,
        .dealReference = "ueur-L1719360000000",
    };
}

const ig::PlaceClose kNoClose = [](const ig::TradeCloseObj&,
                                   const ig::OrderContext&) {
    return ig::CloseResult{.status = ig::CloseStatus::Failed,
                           .reason = "unexpected close in this test"};
};

constexpr std::chrono::seconds kInFlightTtl{7};
constexpr std::chrono::seconds kFailureTtl{9};
constexpr std::chrono::seconds kClosedTtl{300};

}  // namespace

TEST_CASE("an accepted open books the deal under IG's reference and keeps "
          "the lock",
          "[orderChannel]") {
    HookRecorder recorder;
    std::vector<ig::TradeOpenObj> placed;
    std::vector<ig::OrderContext> contexts;
    live::OrderChannel channel(
        kTestLookup,
        [&](const ig::TradeOpenObj& order, const ig::OrderContext& context) {
            placed.push_back(order);
            contexts.push_back(context);
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-1"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK(channel.request(makeRequest()));

    // The cluster gate ran first, the lock was extended (in-flight TTL)
    // BEFORE the broker was called, then the deal was saved (PO#), listed
    // (PL#) and receipted — and never released.
    REQUIRE(recorder.sequence
            == std::vector<std::string>{"cluster", "extend", "save", "add",
                                        "receipt"});
    REQUIRE(recorder.extends.size() == 1);
    CHECK(recorder.extends[0].uuid == "u-eur");
    CHECK(recorder.extends[0].direction == "LONG");
    CHECK(recorder.extends[0].ttl == kInFlightTtl);

    // The TradeOpenObj carries the market definition and the modified size;
    // the context carries what the request gate and audit trail need. The
    // epic must be the MINI contract, never the CFD one.
    REQUIRE(placed.size() == 1);
    CHECK(placed[0].epic == "TEST.EPIC.MINI.IP");
    CHECK(placed[0].currencyCode == "USD");
    CHECK(placed[0].direction == "BUY");   // engine LONG -> broker BUY
    CHECK(placed[0].size == 1.5);          // TRADING_SIZE 3 * sizeModifier 0.5
    CHECK(placed[0].expiry == "-");
    CHECK(placed[0].orderType == "MARKET");
    CHECK(placed[0].forceOpen);
    CHECK_FALSE(placed[0].guaranteedStop);
    CHECK(placed[0].stopDistance == 25);
    CHECK(placed[0].limitDistance == 50);
    CHECK(placed[0].dealReference == makeRequest().dealReference);
    REQUIRE(contexts.size() == 1);
    CHECK(contexts[0].strategyUuid == "u-eur");
    CHECK(contexts[0].symbol == "EURUSD");
    CHECK(contexts[0].openDirection == "BUY");

    // Booked under the BROKER's echoed reference, not the one we minted.
    REQUIRE(recorder.saves.size() == 1);
    CHECK(recorder.saves[0].first == "IGREF-1");
    REQUIRE(recorder.adds.size() == 1);
    CHECK(recorder.adds[0]
          == std::pair<std::string, std::string>{"u-eur", "IGREF-1"});
    REQUIRE(recorder.receipts.size() == 1);
    CHECK(recorder.receipts[0].first == "IGREF-1#EURUSD");
}

TEST_CASE("an accepted open with no echoed reference books under ours",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Accepted};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    const live::OrderRequest request = makeRequest();
    CHECK(channel.request(request));
    REQUIRE(recorder.saves.size() == 1);
    CHECK(recorder.saves[0].first == request.dealReference);
}

TEST_CASE("a SHORT request trades SELL but locks under SHORT",
          "[orderChannel]") {
    HookRecorder recorder;
    std::optional<std::string> brokerDirection;
    live::OrderChannel channel(
        kTestLookup,
        [&brokerDirection](const ig::TradeOpenObj& order,
                           const ig::OrderContext&) {
            brokerDirection = order.direction;
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-2"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK(channel.request(makeRequest(Direction::SHORT)));
    CHECK(brokerDirection == "SELL");
    // The lock hooks speak the engine's direction vocabulary — the SAME
    // strings the runner's gate acquired with (LOCK#<uuid>#<LONG|SHORT>).
    REQUIRE(recorder.extends.size() == 1);
    CHECK(recorder.extends[0].direction == "SHORT");
}

TEST_CASE("a rejected open releases the lock and records nothing",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Rejected,
                                  .reason = "insufficient margin"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.request(makeRequest()));
    CHECK(recorder.sequence
          == std::vector<std::string>{"cluster", "extend", "release"});
    REQUIRE(recorder.releases.size() == 1);
    CHECK(recorder.releases[0]
          == std::pair<std::string, std::string>{"u-eur", "LONG"});
    CHECK(recorder.saves.empty());
    CHECK(recorder.adds.empty());
}

TEST_CASE("a failed open extends the lock for the failure TTL",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Failed,
                                  .reason = "timeout"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.request(makeRequest()));
    // In-flight extend, then the C# two-minute-style failure extend; the
    // order MAY still be live at the broker so the lock must NOT be released.
    CHECK(recorder.sequence
          == std::vector<std::string>{"cluster", "extend", "extend"});
    REQUIRE(recorder.extends.size() == 2);
    CHECK(recorder.extends[1].ttl == kFailureTtl);
    CHECK(recorder.releases.empty());
    CHECK(recorder.saves.empty());
}

TEST_CASE("a throwing broker call takes the failed path, not the process down",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) -> ig::OpenResult {
            throw std::runtime_error("socket reset");
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.request(makeRequest()));
    CHECK(recorder.sequence
          == std::vector<std::string>{"cluster", "extend", "extend"});
    REQUIRE(recorder.extends.size() == 2);
    CHECK(recorder.extends[1].ttl == kFailureTtl);
}

TEST_CASE("a symbol missing from the market definitions never reaches the "
          "broker or the lock hooks",
          "[orderChannel]") {
    HookRecorder recorder;
    int brokerCalls = 0;
    live::OrderChannel channel(
        kTestLookup,
        [&brokerCalls](const ig::TradeOpenObj&, const ig::OrderContext&) {
            ++brokerCalls;
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-X"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.request(makeRequest(Direction::LONG, "NOTLISTED")));
    CHECK(brokerCalls == 0);
    // C# semantics: the signal is dropped and the gate's lock runs out its
    // own TTL — no cluster check, no extend, no release.
    CHECK(recorder.sequence.empty());
}

TEST_CASE("a blocked cluster gate drops the signal and leaves the lock alone",
          "[orderChannel]") {
    HookRecorder recorder;
    recorder.clusterAllow = false;
    int brokerCalls = 0;
    live::OrderChannel channel(
        kTestLookup,
        [&brokerCalls](const ig::TradeOpenObj&, const ig::OrderContext&) {
            ++brokerCalls;
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-X"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.request(makeRequest()));
    // C# semantics: the blocked open never reaches the broker, and the
    // gate's trade lock is neither extended nor released — it runs out its
    // own TTL, throttling re-signals while the cluster stays busy.
    CHECK(brokerCalls == 0);
    CHECK(recorder.sequence == std::vector<std::string>{"cluster"});
    CHECK(recorder.extends.empty());
    CHECK(recorder.releases.empty());
    CHECK(recorder.saves.empty());
}

TEST_CASE("the cluster gate is consulted with the symbol and strategy name",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-1"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK(channel.request(makeRequest()));
    REQUIRE(recorder.clusterChecks.size() == 1);
    CHECK(recorder.clusterChecks[0]
          == std::pair<std::string, std::string>{"EURUSD", "StubStrategy"});
}

TEST_CASE("bookkeeping failure after an accepted open still reports success",
          "[orderChannel]") {
    HookRecorder recorder;
    recorder.saveOk = false;  // Redis died between the fill and the PO# write
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-3"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    // The deal is LIVE at the broker whatever Redis says; the position
    // producer rebuilds PL#/PO# from the broker book on its next refresh.
    CHECK(channel.request(makeRequest()));
    REQUIRE(recorder.adds.size() == 1);  // PL# append still attempted
}

TEST_CASE("the position payload pins the fields the tooling greps for",
          "[orderChannel]") {
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
    const std::string payload =
        live::buildPositionPayload(request, order, "IGREF-9", "");

    CHECK(payload.contains(R"("dealId":"")"));  // confirm never resolved
    CHECK(payload.contains(R"("dealReference":"IGREF-9")"));
    CHECK(payload.contains(R"("strategyId":"u-eur")"));
    CHECK(payload.contains(R"("symbol":"EURUSD")"));
    CHECK(payload.contains(R"("direction":"BUY")"));
    CHECK(payload.contains(R"("level":110002)"));

    const std::string receipt = live::buildDealReceipt(request, "IGREF-9", "");
    CHECK(receipt.contains(R"("id":"DealId#IGREF-9")"));
    CHECK(receipt.contains(R"("sort":"EURUSD")"));
    CHECK(receipt.contains(R"("dealReference":"IGREF-9")"));
    CHECK(receipt.contains(R"("strategyId":"u-eur")"));
    CHECK(receipt.contains(R"("strategyName":"StubStrategy")"));
    CHECK(receipt.contains(R"("dealId":"")"));
    CHECK(receipt.contains(R"("date":"2024-06-26T00:00:00Z")"));
}

TEST_CASE("a confirmed dealId lands in the PO# payload and the receipt",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Accepted,
                                  .dealReference = "IGREF-4",
                                  .dealId = "DIAAA-77"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK(channel.request(makeRequest()));
    REQUIRE(recorder.saves.size() == 1);
    CHECK(recorder.saves[0].second.contains(R"("dealId":"DIAAA-77")"));
    REQUIRE(recorder.receipts.size() == 1);
    CHECK(recorder.receipts[0].second.contains(R"("dealId":"DIAAA-77")"));
}

TEST_CASE("a confirm-driven rejection still releases the lock",
          "[orderChannel]") {
    // The confirms poll surfaces REJECTED through the same OpenStatus the
    // channel already handles — early lock release, nothing booked.
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{.status = ig::OpenStatus::Rejected,
                                  .dealReference = "IGREF-5",
                                  .reason = "INSUFFICIENT_FUNDS"};
        },
        kNoClose, recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.request(makeRequest()));
    CHECK(recorder.sequence
          == std::vector<std::string>{"cluster", "extend", "release"});
    CHECK(recorder.saves.empty());
}

TEST_CASE("closing a LONG position sells it back and prunes the book",
          "[orderChannel]") {
    HookRecorder recorder;
    std::vector<ig::TradeCloseObj> closes;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{};
        },
        [&closes](const ig::TradeCloseObj& close, const ig::OrderContext&) {
            closes.push_back(close);
            return ig::CloseResult{.status = ig::CloseStatus::Ok};
        },
        recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK(channel.closePosition(makeCloseRequest()));

    REQUIRE(closes.size() == 1);
    CHECK(closes[0].direction == "SELL");  // flip of the open BUY
    CHECK(closes[0].orderType == "MARKET");
    CHECK(closes[0].dealId == "DEAL-77");
    CHECK(closes[0].size == 1.5);
    REQUIRE(recorder.removes.size() == 1);
    CHECK(recorder.removes[0]
          == std::pair<std::string, std::string>{"u-eur",
                                                 "ueur-L1719360000000"});

    // The recently-closed window: a second close of the same deal is
    // suppressed without touching the broker again.
    CHECK_FALSE(channel.closePosition(makeCloseRequest()));
    CHECK(closes.size() == 1);
}

TEST_CASE("a close without a dealId never reaches the broker",
          "[orderChannel]") {
    HookRecorder recorder;
    int closeCalls = 0;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{};
        },
        [&closeCalls](const ig::TradeCloseObj&, const ig::OrderContext&) {
            ++closeCalls;
            return ig::CloseResult{.status = ig::CloseStatus::Ok};
        },
        recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.closePosition(makeCloseRequest("")));
    CHECK(closeCalls == 0);
    CHECK(recorder.removes.empty());
}

TEST_CASE("a close with no broker response drops the phantom book entry",
          "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{};
        },
        [](const ig::TradeCloseObj&, const ig::OrderContext&) {
            return ig::CloseResult{.status = ig::CloseStatus::Gone,
                                   .reason = "no response"};
        },
        recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    // Reported as NOT closed, but the book entry is removed — the C#
    // missing-from-IG branch.
    CHECK_FALSE(channel.closePosition(makeCloseRequest()));
    REQUIRE(recorder.removes.size() == 1);

    // Gone does NOT enter the recently-closed window: a retry may reach IG.
    CHECK_FALSE(channel.closePosition(makeCloseRequest()));
    CHECK(recorder.removes.size() == 2);
}

TEST_CASE("a failed close keeps the book entry", "[orderChannel]") {
    HookRecorder recorder;
    live::OrderChannel channel(
        kTestLookup,
        [](const ig::TradeOpenObj&, const ig::OrderContext&) {
            return ig::OpenResult{};
        },
        [](const ig::TradeCloseObj&, const ig::OrderContext&) {
            return ig::CloseResult{.status = ig::CloseStatus::Failed,
                                   .reason = "HTTP 500"};
        },
        recorder.hooks(), kInFlightTtl, kFailureTtl, kClosedTtl);

    CHECK_FALSE(channel.closePosition(makeCloseRequest()));
    CHECK(recorder.removes.empty());
}

TEST_CASE("every priced symbol is tradable and vice versa", "[orderChannel]") {
    // The market table and the symbolScale price table must cover the SAME
    // symbols: a priced symbol with no market silently never trades, and a
    // market with no scale can't even build an OrderRequest.
    CHECK(live::kMarkets.size() == symbol_scale::kTable.size());
    for (const auto& entry : symbol_scale::kTable) {
        INFO(entry.symbol);
        CHECK(live::findMarket(entry.symbol) != nullptr);
    }
    for (const auto& market : live::kMarkets) {
        INFO(market.symbol);
        CHECK(symbol_scale::get(market.symbol) != symbol_scale::kUnknown);
    }
    CHECK(live::findMarket("NOSUCHSYM") == nullptr);
    CHECK(live::findMarket("") == nullptr);
}

TEST_CASE("the market table carries the C# production wire contract",
          "[orderChannel]") {
    // These values come verbatim from the C# engine's live
    // MarketDescriptions — a broker wire contract, so the load-bearing
    // entries are pinned (unlike strategy config, which tests leave free).
    const auto* eurusd = live::findMarket("EURUSD");
    REQUIRE(eurusd != nullptr);
    CHECK(eurusd->epicMini == "CS.D.EURUSD.MINI.IP");
    CHECK(eurusd->epicCfd == "CS.D.EURUSD.CFD.IP");
    CHECK(eurusd->currency == "USD");
    CHECK(eurusd->tradeSizeModifier == 0.0);  // C# null...
    CHECK(eurusd->sizeModifier() == 1.0);     // ...means unscaled

    // The two markets the C# config scales down. XAUUSD's C# 0.5 was raised
    // to 1.0 — IG rejects size 0.5 on that epic as below the market minimum.
    const auto* silver = live::findMarket("XAGUSD");
    REQUIRE(silver != nullptr);
    CHECK(silver->epicMini == "CS.D.CFDSILVER.CFM.IP");
    CHECK(silver->sizeModifier() == 0.2);
    const auto* gold = live::findMarket("XAUUSD");
    REQUIRE(gold != nullptr);
    CHECK(gold->epicMini == "CS.D.CFPGOLD.CFP.IP");
    CHECK(gold->currency == "GBP");  // yes — GBP on this account
    CHECK(gold->sizeModifier() == 1.0);

    // The one market whose mini epic differs from its CFD epic — picking
    // the wrong one doubles the exposure the modifier halves.
    const auto* ftse = live::findMarket("GBRIDXGBP");
    REQUIRE(ftse != nullptr);
    CHECK(ftse->epicMini == "IX.D.FTSE.IFM.IP");
    CHECK(ftse->epicCfd == "IX.D.FTSE.CFD.IP");
    CHECK(ftse->sizeModifier() == 0.5);
}
