// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// StrategyRunner machinery tests: symbol routing, per-worker fan-out, the
// drain-on-stop guarantee, and the gate/sink seam. The gate and sink are
// injected functors, so no Redis (and no Elasticsearch) is involved. All
// counters are read after stop() — the join gives the happens-before edge.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>  // setenv — keep the ATR gate test off QuestDB
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "shared/tradingDefinitions/variables/tradingVariables.hpp"

import barStore;  // bars::BarStore / bars::SeriesSpec
import liveStrategyRunner;
import priceData;
import strategy;
import trade;
import tradeManager;

namespace {

// Deterministic IStrategy: a fixed decide() answer, call counting for both
// hooks. Owned by the runner via WorkerSpec; the test keeps a raw pointer.
class StubStrategy final : public IStrategy {
public:
    explicit StubStrategy(std::optional<Direction> signal) : signal_(signal) {}

    std::optional<Direction> decide(const PriceData& /*tick*/,
                                    const bars::BarStore& /*barStore*/) override {
        ++decideCalls;
        return signal_;
    }

    void during(const PriceData& /*price*/, const bars::BarStore& /*barStore*/,
                TradeManager& /*tm*/) override {
        ++duringCalls;
    }

    int decideCalls = 0;
    int duringCalls = 0;

private:
    std::optional<Direction> signal_;
};

// With the ATR gate disengaged (these specs set no gateSeries), the values
// flow into OrderIntent literally, so the exact-value assertions below hold.
tradingDefinitions::TradingVariables stubVars() {
    tradingDefinitions::TradingVariables vars;
    vars.STRATEGY = "StubStrategy";
    vars.STOP_DISTANCE_IN_ATR = 25;
    vars.LIMIT_DISTANCE_IN_ATR = 50;
    vars.TRADING_SIZE = 3;
    return vars;
}

// Builds a spec and hands back the stub for post-run inspection. Cap
// defaults match WorkerSpec's own (no open-trades cap, per-minute brake on).
std::pair<live::WorkerSpec, StubStrategy*> makeSpec(
    const std::string& symbol, const std::string& uuid,
    std::optional<Direction> signal, const int maxOpenTrades = 0,
    const int maxTradesPerMinute = 60) {
    auto stub = std::make_unique<StubStrategy>(signal);
    StubStrategy* raw = stub.get();
    live::WorkerSpec spec{
        .symbol = symbol,
        .strategyName = "StubStrategy",
        .strategyUuid = uuid,
        .vars = stubVars(),
        .maxOpenTrades = maxOpenTrades,
        .maxTradesPerMinute = maxTradesPerMinute,
        .strategy = std::move(stub),
    };
    return {std::move(spec), raw};
}

PriceData makeTick(const std::string& symbol, const std::int32_t bid = 110000,
                   const std::int32_t ask = 110002,
                   const std::chrono::seconds offset = std::chrono::seconds{0}) {
    return PriceData{ask, bid,
                     std::chrono::system_clock::time_point{
                         std::chrono::microseconds{1'719'360'000'000'000LL}}
                         + offset,
                     symbol};
}

const live::TradeGate kAlwaysOpen = [](const std::string&, const std::string&) {
    return true;
};
const live::OrderSink kDiscard = [](const live::OrderIntent&) {};

// Throws a non-std type from decide() — the worker guard must catch this too
// (an escaping non-std exception would std::terminate the whole process).
class NonStdThrowingStrategy final : public IStrategy {
public:
    std::optional<Direction> decide(const PriceData& /*tick*/,
                                    const bars::BarStore& /*barStore*/) override {
        ++decideCalls;
        throw 42;
    }

    void during(const PriceData& /*price*/, const bars::BarStore& /*barStore*/,
                TradeManager& /*tm*/) override {}

    int decideCalls = 0;
};

// Consults the book from during() — the divergence the position feed
// exists to close — recording what it saw; optionally closes the trade,
// which is the close-signal path under test.
class BookStrategy final : public IStrategy {
public:
    explicit BookStrategy(const bool closeWhenActive)
        : closeWhenActive_(closeWhenActive) {}

    std::optional<Direction> decide(const PriceData& /*tick*/,
                                    const bars::BarStore& /*barStore*/) override {
        return std::nullopt;
    }

    void during(const PriceData& price, const bars::BarStore& /*barStore*/,
                TradeManager& tradeManager) override {
        if (const Trade* trade = tradeManager.findActiveTrade(price.symbol)) {
            ++sawActive;
            seenEntryPrice = trade->entryPrice;
            seenSize = trade->size;
            seenDirection = trade->direction;
            if (closeWhenActive_) {
                tradeManager.closeTrade(price.symbol, trade->lastMarkPrice,
                                        price);
            }
        }
    }

    int sawActive = 0;
    std::int32_t seenEntryPrice = 0;
    std::int32_t seenSize = 0;
    Direction seenDirection = Direction::LONG;

private:
    bool closeWhenActive_;
};

std::pair<live::WorkerSpec, BookStrategy*> makeBookSpec(
    const std::string& symbol, const std::string& uuid,
    const bool closeWhenActive) {
    auto strategy = std::make_unique<BookStrategy>(closeWhenActive);
    BookStrategy* raw = strategy.get();
    live::WorkerSpec spec{
        .symbol = symbol,
        .strategyName = "BookStrategy",
        .strategyUuid = uuid,
        .vars = stubVars(),
        .strategy = std::move(strategy),
    };
    return {std::move(spec), raw};
}

live::BookedPosition makeBooked(const std::string& dealId = "DIAAA-1") {
    return live::BookedPosition{
        .dealId = dealId,
        .dealReference = "IGREF-1",
        .direction = Direction::LONG,
        .brokerSize = 1.5,
        .engineSize = 3,
        .level = 110002,
        .openedAt = std::chrono::system_clock::time_point{
            std::chrono::microseconds{1'719'360'000'000'000LL}},
    };
}

// Scripted PositionFeed: one entry per sync (the last repeats), argument
// capture for the per-worker scoping. Worker-thread-only writes; read after
// stop() joins.
struct ScriptedFeed {
    std::vector<std::optional<std::vector<live::BookedPosition>>> script;
    std::vector<std::pair<std::string, std::string>> calls;  // uuid, symbol

    live::PositionFeed fn() {
        return [this](const std::string& uuid, const std::string& symbol) {
            calls.emplace_back(uuid, symbol);
            const std::size_t index =
                std::min(calls.size() - 1, script.size() - 1);
            return script[index];
        };
    }
};

const live::CloseSink kDiscardClose = [](const live::CloseIntent&) {};

}  // namespace

TEST_CASE("StrategyRunner routes ticks by symbol and counts uncached symbols",
          "[liveRunner]") {
    auto [eurSpec, eurStub] = makeSpec("EURUSD", "u-eur", std::nullopt);
    auto [jpySpec, jpyStub] = makeSpec("USDJPY", "u-jpy", std::nullopt);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(eurSpec));
    specs.push_back(std::move(jpySpec));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("GBPUSD"));  // no worker cached for this symbol
    runner.stop();

    CHECK(eurStub->duringCalls == 3);
    CHECK(eurStub->decideCalls == 3);
    CHECK(jpyStub->duringCalls == 0);

    const live::RunnerStats stats = runner.stats();
    CHECK(stats.routed == 3);
    CHECK(stats.ignoredSymbol == 1);
    CHECK(stats.queueDropped == 0);
}

TEST_CASE("StrategyRunner fans one tick out to every worker on the symbol",
          "[liveRunner]") {
    auto [specA, stubA] = makeSpec("EURUSD", "u-a", std::nullopt);
    auto [specB, stubB] = makeSpec("EURUSD", "u-b", std::nullopt);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(specA));
    specs.push_back(std::move(specB));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    for (int i = 0; i < 5; ++i) {
        runner.onTick(makeTick("EURUSD"));
    }
    runner.stop();

    CHECK(stubA->duringCalls == 5);
    CHECK(stubB->duringCalls == 5);
    CHECK(runner.stats().routed == 10);  // per-worker deliveries
}

TEST_CASE("stop() drains queued ticks before the workers exit", "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-drain", std::nullopt);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    // Enqueue before start: the whole backlog sits in the queue, so this only
    // passes if stop() really drains rather than aborting mid-queue.
    constexpr int kTicks = 200;
    for (int i = 0; i < kTicks; ++i) {
        runner.onTick(makeTick("EURUSD"));
    }
    runner.start();
    runner.stop();

    CHECK(stub->duringCalls == kTicks);
    CHECK(runner.stats().routed == kTicks);
}

TEST_CASE("StrategyRunner processes ticks again after a stop()/start() cycle",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-restart", std::nullopt);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    runner.start();
    // Give a regressed runner (stop flag never reset) time to let its fresh
    // workers drain-and-exit before the tick arrives; a correct runner's
    // workers are parked on the condition variable, so this costs nothing
    // but the sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    CHECK(stub->duringCalls == 3);
    CHECK(runner.stats().routed == 3);
}

TEST_CASE("a non-std exception from a strategy does not kill the worker",
          "[liveRunner]") {
    auto strategy = std::make_unique<NonStdThrowingStrategy>();
    NonStdThrowingStrategy* raw = strategy.get();
    live::WorkerSpec spec{
        .symbol = "EURUSD",
        .strategyName = "NonStdThrowingStrategy",
        .strategyUuid = "u-throw",
        .vars = stubVars(),
        .strategy = std::move(strategy),
    };
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    // Three throws, three survivals: the worker kept draining its queue.
    CHECK(raw->decideCalls == 3);
    CHECK(runner.stats().routed == 3);
}

TEST_CASE("a closed gate blocks the order and counts lockBlocked",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-gated", Direction::LONG);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    // Counters are only read after stop() joins the worker, so plain ints
    // behind a reference are safe here.
    int gateCalls = 0;
    int sinkCalls = 0;
    live::TradeGate gate = [&gateCalls](const std::string&, const std::string&) {
        ++gateCalls;
        return false;  // lock held (or Redis fail-closed)
    };
    live::OrderSink sink = [&sinkCalls](const live::OrderIntent&) { ++sinkCalls; };

    live::StrategyRunner runner(std::move(specs), std::move(gate), std::move(sink));
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    CHECK(gateCalls == 3);
    CHECK(sinkCalls == 0);
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.signals == 3);
    CHECK(stats.lockBlocked == 3);
    CHECK(stats.ordersLogged == 0);
    CHECK(stub->duringCalls == 3);  // during still runs when the gate blocks
}

TEST_CASE("an open gate emits a fully-populated OrderIntent", "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-order", Direction::SHORT);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    std::vector<std::pair<std::string, std::string>> gateArgs;
    std::vector<live::OrderIntent> orders;
    live::TradeGate gate = [&gateArgs](const std::string& uuid,
                                       const std::string& direction) {
        gateArgs.emplace_back(uuid, direction);
        return true;
    };
    live::OrderSink sink = [&orders](const live::OrderIntent& order) {
        orders.push_back(order);
    };

    live::StrategyRunner runner(std::move(specs), std::move(gate), std::move(sink));
    runner.start();
    runner.onTick(makeTick("EURUSD", /*bid=*/110010, /*ask=*/110013));
    runner.stop();

    REQUIRE(gateArgs.size() == 1);
    CHECK(gateArgs[0].first == "u-order");
    CHECK(gateArgs[0].second == "SHORT");

    REQUIRE(orders.size() == 1);
    const live::OrderIntent& order = orders[0];
    CHECK(order.strategyName == "StubStrategy");
    CHECK(order.strategyUuid == "u-order");
    CHECK(order.symbol == "EURUSD");
    CHECK(order.direction == Direction::SHORT);
    CHECK(order.size == 3);
    CHECK(order.stopDistancePips == 25);
    CHECK(order.limitDistancePips == 50);
    CHECK(order.bid == 110010);
    CHECK(order.ask == 110013);

    const live::RunnerStats stats = runner.stats();
    CHECK(stats.signals == 1);
    CHECK(stats.ordersLogged == 1);
    CHECK(stats.lockBlocked == 0);
}

// Peak-hours entry gate: out-of-session ticks skip decide() entirely (counted
// as sessionSkipped) while during() still runs; an in-session tick trades as
// normal. The fixed tick epoch is Wed 2024-06-26 00:00 UTC — under BST, so
// EURUSD's Europe window is 07:00-10:00 UTC.
TEST_CASE("peakHoursOnly skips decide() outside the session window",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-session", Direction::LONG);
    spec.peakHoursOnly = true;
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    std::vector<live::OrderIntent> orders;
    live::OrderSink sink = [&orders](const live::OrderIntent& order) {
        orders.push_back(order);
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, std::move(sink));
    runner.start();
    runner.onTick(makeTick("EURUSD"));  // 00:00 UTC — out of session
    runner.onTick(makeTick("EURUSD", 110000, 110002,
                           std::chrono::hours{5}));  // 05:00 — out
    runner.onTick(makeTick("EURUSD", 110000, 110002,
                           std::chrono::hours{8}));  // 08:00 — in
    runner.stop();

    CHECK(stub->decideCalls == 1);  // only the 08:00 tick reached decide()
    CHECK(stub->duringCalls == 3);  // during() is never gated

    REQUIRE(orders.size() == 1);
    CHECK(orders[0].timestamp ==
          makeTick("EURUSD", 110000, 110002, std::chrono::hours{8}).timestamp);

    const live::RunnerStats stats = runner.stats();
    CHECK(stats.sessionSkipped == 2);
    CHECK(stats.signals == 1);
    CHECK(stats.ordersLogged == 1);
}

// The gate covers entries only: on an out-of-session tick the book still
// syncs and a strategy close from during() still flows out as a CloseIntent
// (mirrors the strategy-close test below, with the filter on).
TEST_CASE("peakHoursOnly never gates the book sync or strategy closes",
          "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-session-close",
                                         /*closeWhenActive=*/true);
    spec.peakHoursOnly = true;
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{makeBooked()},
                   std::vector<live::BookedPosition>{}};  // gone after close
    std::vector<live::CloseIntent> intents;
    live::CloseSink closeSink = [&intents](const live::CloseIntent& intent) {
        intents.push_back(intent);
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), std::move(closeSink),
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("EURUSD"));  // 00:00 UTC — out of session
    runner.stop();

    REQUIRE(intents.size() == 1);
    CHECK(intents[0].dealId == "DIAAA-1");
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.sessionSkipped == 1);   // the entry path was gated...
    CHECK(stats.bookSeeded == 1);       // ...but the sync still seeded
    CHECK(stats.strategyCloses == 1);   // ...and the close still flowed out
}

TEST_CASE("MAX_TRADES_PER_MINUTE caps entries in a sliding tick-time window",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-rate", Direction::LONG,
                                 /*maxOpenTrades=*/0, /*maxTradesPerMinute=*/2);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    using std::chrono::seconds;
    runner.onTick(makeTick("EURUSD", 110000, 110002, seconds{0}));   // order 1
    runner.onTick(makeTick("EURUSD", 110000, 110002, seconds{1}));   // order 2
    runner.onTick(makeTick("EURUSD", 110000, 110002, seconds{2}));   // window full
    // 60s after the first order: the half-open window has aged it out, so a
    // slot is free again — the same boundary the backtest cap uses.
    runner.onTick(makeTick("EURUSD", 110000, 110002, seconds{60}));  // order 3
    runner.stop();

    const live::RunnerStats stats = runner.stats();
    CHECK(stats.signals == 4);
    CHECK(stats.ordersLogged == 3);
    CHECK(stats.rateBlocked == 1);
    CHECK(stats.lockBlocked == 0);
    CHECK(stub->duringCalls == 4);  // during still runs when the cap blocks
}

TEST_CASE("maxTradesPerMinute <= 0 disables the rate cap", "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-uncapped", Direction::LONG,
                                 /*maxOpenTrades=*/0, /*maxTradesPerMinute=*/0);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    for (int i = 0; i < 5; ++i) {
        runner.onTick(makeTick("EURUSD"));  // all in the same tick instant
    }
    runner.stop();

    const live::RunnerStats stats = runner.stats();
    CHECK(stats.ordersLogged == 5);
    CHECK(stats.rateBlocked == 0);
    CHECK(stub->decideCalls == 5);
}

TEST_CASE("MAX_OPEN_TRADES consults the position counter and fails closed on "
          "an unknown count",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-pos", Direction::LONG,
                                 /*maxOpenTrades=*/2);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    // One scripted count per signal; ticks drain in order on the single
    // worker thread and the vector is only read after stop() joins it.
    std::vector<std::optional<int>> counts{2, 1, std::nullopt};
    std::vector<std::string> counterArgs;
    live::PositionCounter counter =
        [&counts, &counterArgs](const std::string& uuid) {
            counterArgs.push_back(uuid);
            return counts[counterArgs.size() - 1];
        };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard,
                                std::move(counter));
    runner.start();
    runner.onTick(makeTick("EURUSD"));  // count 2, at the cap  -> blocked
    runner.onTick(makeTick("EURUSD"));  // count 1, below       -> order
    runner.onTick(makeTick("EURUSD"));  // unknown (Redis down) -> blocked
    runner.stop();

    REQUIRE(counterArgs.size() == 3);
    CHECK(counterArgs[0] == "u-pos");
    CHECK(stub->decideCalls == 3);
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.signals == 3);
    CHECK(stats.positionBlocked == 2);
    CHECK(stats.ordersLogged == 1);
    CHECK(stats.lockBlocked == 0);
}

TEST_CASE("maxOpenTrades <= 0 never consults the position counter",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-nocap", Direction::LONG,
                                 /*maxOpenTrades=*/0);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    int counterCalls = 0;
    live::PositionCounter counter =
        [&counterCalls](const std::string&) -> std::optional<int> {
        ++counterCalls;
        return 0;
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard,
                                std::move(counter));
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    CHECK(counterCalls == 0);
    CHECK(stub->decideCalls == 2);
    CHECK(runner.stats().ordersLogged == 2);
    CHECK(runner.stats().positionBlocked == 0);
}

TEST_CASE("an open-trades cap with no counter wired fails closed",
          "[liveRunner]") {
    auto [spec, stub] = makeSpec("EURUSD", "u-nocounter", Direction::LONG,
                                 /*maxOpenTrades=*/1);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    // No PositionCounter argument: the position state is unknowable, so a
    // capped spec must block every entry rather than trade ungated.
    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard);
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    CHECK(stub->decideCalls == 2);
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.positionBlocked == 2);
    CHECK(stats.ordersLogged == 0);
}

TEST_CASE("the position feed seeds the book at the booked level",
          "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-book",
                                         /*closeWhenActive=*/false);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{makeBooked()}};
    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), kDiscardClose,
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));  // second sync must NOT re-seed
    runner.stop();

    // The strategy's during() saw a real trade, entered exactly at the
    // booked level (synthetic zero-spread tick) with the engine-lots size.
    CHECK(strategy->sawActive == 2);
    CHECK(strategy->seenEntryPrice == 110002);
    CHECK(strategy->seenSize == 3);
    CHECK(strategy->seenDirection == Direction::LONG);
    REQUIRE(feed.calls.size() == 2);
    CHECK(feed.calls[0]
          == std::pair<std::string, std::string>{"u-book", "EURUSD"});
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.bookSeeded == 1);
    CHECK(stats.bookRemoved == 0);
    CHECK(stats.strategyCloses == 0);
}

TEST_CASE("a deal gone from the feed leaves the book without a close intent",
          "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-gone",
                                         /*closeWhenActive=*/false);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{makeBooked()},
                   std::vector<live::BookedPosition>{}};  // closed at broker
    int closeIntents = 0;
    live::CloseSink closeSink = [&closeIntents](const live::CloseIntent&) {
        ++closeIntents;
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), std::move(closeSink),
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("EURUSD"));  // seeds
    runner.onTick(makeTick("EURUSD"));  // removal — broker closed it
    runner.stop();

    CHECK(strategy->sawActive == 1);  // gone before the second during()
    CHECK(closeIntents == 0);         // broker closes never echo back out
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.bookSeeded == 1);
    CHECK(stats.bookRemoved == 1);
    CHECK(stats.strategyCloses == 0);
}

TEST_CASE("an unknown feed keeps the book unchanged", "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-blip",
                                         /*closeWhenActive=*/false);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{makeBooked()},
                   std::nullopt};  // Redis blip: state UNKNOWN
    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), kDiscardClose,
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    // Fail-open for book state: the trade survived the blip.
    CHECK(strategy->sawActive == 2);
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.bookSeeded == 1);
    CHECK(stats.bookRemoved == 0);
    CHECK(stats.bookSyncFailed == 1);
}

TEST_CASE("the book sync respects its minimum interval", "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-cadence",
                                         /*closeWhenActive=*/false);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{}};
    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), kDiscardClose,
                                std::chrono::hours{1});
    runner.start();
    for (int i = 0; i < 5; ++i) {
        runner.onTick(makeTick("EURUSD"));
    }
    runner.stop();

    CHECK(feed.calls.size() == 1);  // first tick syncs, the rest are inside
                                    // the interval
}

TEST_CASE("a strategy close emits a CloseIntent carrying the booked deal",
          "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-close",
                                         /*closeWhenActive=*/true);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{makeBooked()},
                   std::vector<live::BookedPosition>{}};  // gone after close
    std::vector<live::CloseIntent> intents;
    live::CloseSink closeSink = [&intents](const live::CloseIntent& intent) {
        intents.push_back(intent);
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), std::move(closeSink),
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("EURUSD"));  // seed, then during() closes
    runner.onTick(makeTick("EURUSD"));  // book empty — nothing more happens
    runner.stop();

    REQUIRE(intents.size() == 1);
    const live::CloseIntent& intent = intents[0];
    CHECK(intent.strategyName == "BookStrategy");
    CHECK(intent.strategyUuid == "u-close");
    CHECK(intent.symbol == "EURUSD");
    CHECK(intent.direction == Direction::LONG);  // the OPEN direction
    CHECK(intent.brokerSize == 1.5);             // exact broker units
    CHECK(intent.dealId == "DIAAA-1");
    CHECK(intent.dealReference == "IGREF-1");
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.strategyCloses == 1);
    CHECK(stats.closeDropped == 0);
    CHECK(stats.bookRemoved == 0);  // the strategy closed it, not the sync
}

TEST_CASE("a strategy close without a booked dealId is dropped and counted",
          "[liveRunner]") {
    auto [spec, strategy] = makeBookSpec("EURUSD", "u-unconfirmed",
                                         /*closeWhenActive=*/true);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    // The deal never confirmed — booked with an empty dealId; then gone (so
    // the second tick cannot re-seed and re-close).
    feed.script = {std::vector<live::BookedPosition>{makeBooked("")},
                   std::vector<live::BookedPosition>{}};
    int closeIntents = 0;
    live::CloseSink closeSink = [&closeIntents](const live::CloseIntent&) {
        ++closeIntents;
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), std::move(closeSink),
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("EURUSD"));
    runner.stop();

    CHECK(closeIntents == 0);  // nothing addressable at the broker
    const live::RunnerStats stats = runner.stats();
    CHECK(stats.closeDropped == 1);
    CHECK(stats.strategyCloses == 0);
}

TEST_CASE("a feed for another symbol's deals books nothing", "[liveRunner]") {
    // Belt-and-braces on the per-worker symbol scoping: the feed adapter
    // filters by symbol, so a worker whose symbol has no deals gets an
    // empty vector — which must seed nothing.
    auto [spec, strategy] = makeBookSpec("USDJPY", "u-other",
                                         /*closeWhenActive=*/false);
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    ScriptedFeed feed;
    feed.script = {std::vector<live::BookedPosition>{}};
    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, kDiscard, {},
                                feed.fn(), kDiscardClose,
                                std::chrono::seconds{0});
    runner.start();
    runner.onTick(makeTick("USDJPY"));
    runner.stop();

    REQUIRE(feed.calls.size() == 1);
    CHECK(feed.calls[0].second == "USDJPY");  // the worker asks for ITS symbol
    CHECK(strategy->sawActive == 0);
    CHECK(runner.stats().bookSeeded == 0);
}

// ATR entry conditions: with gateSeries engaged, signals on a cold worker are
// skipped BEFORE decide() (counted as conditionsSkipped); once the gate
// series warms, orders carry the dynamic ATR-derived pip distances instead of
// the raw multipliers.
TEST_CASE("ATR entry gate skips cold workers then emits dynamic distances",
          "[liveRunner]") {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query

    auto [spec, stub] = makeSpec("EURUSD", "u-atr", Direction::LONG);
    spec.vars.STOP_DISTANCE_IN_ATR = 1;
    spec.vars.LIMIT_DISTANCE_IN_ATR = 3;
    spec.gateSeries = bars::SeriesSpec{std::chrono::minutes{15}, 11};
    std::vector<live::WorkerSpec> specs;
    specs.push_back(std::move(spec));

    std::vector<live::OrderIntent> orders;
    live::OrderSink sink = [&orders](const live::OrderIntent& order) {
        orders.push_back(order);
    };

    live::StrategyRunner runner(std::move(specs), kAlwaysOpen, std::move(sink));
    runner.start();
    // Zero-spread ticks 16 minutes apart: each rolls a fresh 15m bar
    // (calculateOHLC rolls on STRICTLY-greater than the duration) and steps
    // the price 200 points (20 EURUSD pips), so every true range is 200 and
    // ATR(10) reads exactly 200 points once 11 bars exist. The store updates
    // at the TOP of processTick, so the gate on tick i sees i+1 bars: ticks
    // 0-9 are the cold phase and ticks 10-11 are warm.
    constexpr int kTicks = 12;
    for (int i = 0; i < kTicks; ++i) {
        const std::int32_t price = 110000 + i * 200;
        runner.onTick(
            makeTick("EURUSD", price, price, std::chrono::minutes{16 * i}));
    }
    runner.stop();

    // Warm ticks: stop = 20 pips x 1, limit = 20 pips x 3 — the dynamic
    // distances, not the raw multipliers. (No open-position gate in the
    // runner, so both warm ticks emit.)
    CHECK(stub->decideCalls == 2);
    REQUIRE(orders.size() == 2);
    CHECK(orders[0].stopDistancePips == 20);
    CHECK(orders[0].limitDistancePips == 60);
    CHECK(orders[1].stopDistancePips == 20);
    CHECK(orders[1].limitDistancePips == 60);

    const live::RunnerStats stats = runner.stats();
    CHECK(stats.conditionsSkipped == 10);
    CHECK(stats.signals == 2);
    CHECK(stats.ordersLogged == 2);
}
