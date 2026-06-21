// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>            // setenv — disable Elastic reporting for run() smoke tests
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/decimal/literals.hpp>

#include "shared/tradingDefinitions/configuration.hpp"

import tradeManager;        // TradeManager
import exitRules;           // trading::exit_rules::checkExit
import reviewStopAndLimit;  // trading::reviewStopAndLimit
import runLoop;             // trading::runTicks, RiskLimits, RunStatus
import operations;          // Operations::run
import strategy;            // IStrategy
import priceData;           // PriceData
import trade;               // Direction, Trade

// Pulls in the _dd user-defined literal so "1.23"_dd produces a decimal64_t
// directly. decimal64_t has no implicit conversion from double — the closest
// C# analogue is having to write `1.23m` instead of `1.23` for a decimal. PnL,
// size, balances and pip distances are still decimal; prices are not.
//
// Prices are scaled fixed-point INT32 now: the real price times the symbol's
// multiplier (EURUSD x100000, AUSIDXAUD x100 — see symbolScale). So EURUSD
// 1.10010 is 110010 and AUSIDXAUD 7000.50 is 700050. SL/TP distances are in the
// same integer price points, so 10 points on EURUSD == 0.0001 == one classic
// pip, and realized PnL is simply priceDiff_int * size.
using namespace boost::decimal::literals;

namespace {

// Minimal Configuration that drives Operations::run down the RandomStrategy
// path. selectStrategy only reads STRATEGY.TRADING_VARIABLES.STRATEGY, and
// RandomStrategy ignores everything else in the strategy block, so the OHLC
// and strategy-variable sections are left default-constructed. The pip / size
// values are plausible but irrelevant to the no-throw assertions below.
tradingDefinitions::Configuration makeRandomStrategyConfig() {
    tradingDefinitions::Configuration config;
    config.RUN_ID = "TEST_RUN";
    config.SYMBOLS = "EURUSD";
    config.LAST_MONTHS = 1;
    config.STRATEGY.UUID = "test-strategy-uuid";
    auto& vars = config.STRATEGY.TRADING_VARIABLES;
    vars.STRATEGY = "RandomStrategy";
    vars.STOP_DISTANCE_IN_PIPS = 10;
    vars.LIMIT_DISTANCE_IN_PIPS = 10;
    vars.TRADING_SIZE = 1;
    return config;
}

// Per-test TradingVariables so each runTicks case can dial SL/TP independently
// (e.g. stop=0 to isolate the take-profit path). STRATEGY is unused by runTicks.
tradingDefinitions::TradingVariables makeVars(int32_t stopPips,
                                               int32_t limitPips,
                                               int32_t size) {
    tradingDefinitions::TradingVariables vars;
    vars.STRATEGY = "Scripted";
    vars.STOP_DISTANCE_IN_PIPS = stopPips;
    vars.LIMIT_DISTANCE_IN_PIPS = limitPips;
    vars.TRADING_SIZE = size;
    return vars;
}

// Deterministic strategy that replays a pre-scripted sequence of signals, one
// per decide() call, returning nullopt ("no trade") once exhausted. This is the
// injectable seam that lets us assert exact bid/ask outcomes from the run loop
// without RandomStrategy's coin flips.
struct ScriptedStrategy : IStrategy {
    std::vector<std::optional<Direction>> script;
    std::size_t index = 0;

    explicit ScriptedStrategy(std::vector<std::optional<Direction>> signals)
        : script(std::move(signals)) {}

    std::optional<Direction> decide(const PriceData& /*tick*/) override {
        return index < script.size() ? script[index++] : std::nullopt;
    }
    void during(const PriceData& /*tick*/, TradeManager& /*tradeManager*/) override {}
};

// Always signals LONG — used to probe re-entry behaviour (gating while a
// position is open; same-tick re-entry after a stop-out).
struct AlwaysLongStrategy : IStrategy {
    std::optional<Direction> decide(const PriceData& /*tick*/) override {
        return Direction::LONG;
    }
    void during(const PriceData& /*tick*/, TradeManager& /*tradeManager*/) override {}
};

}  // namespace

TEST_CASE("TradeManager opens a trade", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(tick, 1, Direction::LONG);
    CHECK_FALSE(tradeId.empty());
    CHECK(manager.reviewAccount() == 1);
}

TEST_CASE("TradeManager closes a trade", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(tick, 1, Direction::LONG);
    bool closed = manager.closeTrade(tradeId, 11000000, tick);
    CHECK(closed);
    CHECK(manager.reviewAccount() == 0);
}

TEST_CASE("TradeManager tracks multiple trades", "[tradeManager]") {
    TradeManager manager;
    PriceData tick1(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick2(20000000, 19900000, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick3(30000000, 29900000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick1, 1, Direction::LONG);
    manager.openTrade(tick2, 2, Direction::SHORT);
    manager.openTrade(tick3, 3, Direction::LONG);

    CHECK(manager.reviewAccount() == 3);
}

TEST_CASE("TradeManager records trade details", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(tick, 1, Direction::LONG);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);

    REQUIRE(trade != trades.end());
    CHECK(trade->second.entryPrice == 10000000);
    CHECK(trade->second.size == 1);
    CHECK(trade->second.direction == Direction::LONG);
}

// Regression: with a 1-pip EURUSD spread and a 1-pip stop, a LONG must not
// be stopped out on its own opening tick. The stop is measured from the
// adverse side (entry bid), not the execution price (entry ask) — otherwise
// the spread alone trips the stop. (1 classic pip == 10 integer points here.)
TEST_CASE("LONG does not exit on entry tick with a one-pip spread", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());
    CHECK(trade->second.entryPrice == 110010);
    CHECK(trade->second.exitReferencePrice == 110000);

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    CHECK_FALSE(exit.has_value());
}

// Symmetric SHORT case: must not be stopped out on the opening tick.
TEST_CASE("SHORT does not exit on entry tick with a one-pip spread", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::SHORT, 1, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());
    CHECK(trade->second.entryPrice == 110000);
    CHECK(trade->second.exitReferencePrice == 110010);

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    CHECK_FALSE(exit.has_value());
}

// Sanity check: once price moves enough, the stop still fires — the fix
// shifts the threshold by one pip, it does not disable exits.
TEST_CASE("LONG stops out when bid drops one pip below entry bid", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);

    PriceData laterTick(110000, 109990, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, laterTick);
    REQUIRE(exit.has_value());
    CHECK(*exit == 109990);
}

// LONG × SL/TP × flat tick: passing the entry tick itself back through
// checkExit must not fire either side. This pins the spread-vs-stop
// invariant: SL is anchored on the entry bid (exitReferencePrice), not
// the execution ask.
TEST_CASE("checkExit: LONG flat tick does not fire", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110011, 110001, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    CHECK_FALSE(exit.has_value());
}

TEST_CASE("checkExit: LONG SL fires at bid", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110011, 110001, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 0);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    // The SL distance is 1 pip (10 points), so entry-bid 110001 minus 10 is
    // 109991; the next-tick bid lands exactly on that level and trips it.
    PriceData nextTick(110001, 109991, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    REQUIRE(exit.has_value());
    CHECK(*exit == nextTick.bid);
    CHECK(*exit == 109991);
}

TEST_CASE("checkExit: SHORT SL fires at ask", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110011, 110001, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::SHORT, 1, 0);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    PriceData nextTick(110021, 110011, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    REQUIRE(exit.has_value());
    CHECK(*exit == nextTick.ask);
    CHECK(*exit == 110021);
}

TEST_CASE("checkExit: LONG TP fires at bid", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110011, 110001, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 0, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    PriceData nextTick(110021, 110011, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    REQUIRE(exit.has_value());
    CHECK(*exit == nextTick.bid);
    CHECK(*exit == 110011);
}

TEST_CASE("checkExit: SHORT TP fires at ask", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110011, 110001, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::SHORT, 0, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    PriceData nextTick(110001, 109991, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    REQUIRE(exit.has_value());
    CHECK(*exit == nextTick.ask);
    CHECK(*exit == 110001);
}

// Cross-symbol tick must not close a EURUSD trade. Without the symbol filter
// in reviewStopAndLimit, an AUSIDXAUD price (~700000) fed through checkExit
// against a EURUSD LONG lands far outside the EURUSD price band, so without the
// guard the trade would close at a nonsensical price.
TEST_CASE("reviewStopAndLimit skips trades for other symbols", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);

    PriceData ausTick(700000, 700000, std::chrono::system_clock::now(), "AUSIDXAUD");
    trading::reviewStopAndLimit(manager, ausTick);

    CHECK(manager.reviewAccount() == 1);
    auto trades = manager.getActiveTrades();
    CHECK(trades.find(tradeId) != trades.end());
}

// Matching-symbol tick still closes — the filter must not over-block exits
// when the tick's symbol matches the trade's symbol.
TEST_CASE("reviewStopAndLimit closes a trade on a matching-symbol tick", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);

    PriceData stopTick(110000, 109990, std::chrono::system_clock::now(), "EURUSD");
    trading::reviewStopAndLimit(manager, stopTick);

    auto active = manager.getActiveTrades();
    CHECK(active.find(tradeId) == active.end());

    const auto& closed = manager.getClosedTrades();
    REQUIRE(closed.size() == 1);
    CHECK(closed.front().id == tradeId);
    CHECK(closed.front().closePrice == stopTick.bid);
    CHECK(closed.front().closePrice == 109990);
}

// Symmetric cross-symbol case: an AUSIDXAUD trade (price ~700000) must not
// close on a EURUSD tick (~110000).
TEST_CASE("reviewStopAndLimit: AUSIDXAUD trade not closed by EURUSD tick", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(700050, 700000, std::chrono::system_clock::now(), "AUSIDXAUD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);

    PriceData eurTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    trading::reviewStopAndLimit(manager, eurTick);

    CHECK(manager.reviewAccount() == 1);
    auto trades = manager.getActiveTrades();
    CHECK(trades.find(tradeId) != trades.end());
}

TEST_CASE("hasActiveTradeForSymbol: empty manager returns false", "[tradeManager]") {
    TradeManager manager;
    CHECK_FALSE(manager.hasActiveTradeForSymbol("EURUSD"));
}

TEST_CASE("hasActiveTradeForSymbol: true for opened symbol, false for other", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick, 1, Direction::LONG);

    CHECK(manager.hasActiveTradeForSymbol("EURUSD"));
    CHECK_FALSE(manager.hasActiveTradeForSymbol("GBPUSD"));
}

TEST_CASE("hasActiveTradeForSymbol: false after closeTrade", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(tick, 1, Direction::LONG);
    REQUIRE(manager.hasActiveTradeForSymbol("EURUSD"));

    bool closed = manager.closeTrade(tradeId, 110000, tick);
    CHECK(closed);
    CHECK_FALSE(manager.hasActiveTradeForSymbol("EURUSD"));
}

TEST_CASE("Can open trades on different symbols simultaneously", "[tradeManager]") {
    TradeManager manager;
    PriceData eurTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(eurTick, 1, Direction::LONG);
    CHECK(manager.hasActiveTradeForSymbol("EURUSD"));
    CHECK_FALSE(manager.hasActiveTradeForSymbol("AUSIDXAUD"));

    PriceData ausTick(700010, 700000, std::chrono::system_clock::now(), "AUSIDXAUD");
    manager.openTrade(ausTick, 1, Direction::LONG);

    CHECK(manager.getActiveTrades().size() == 2);
    CHECK(manager.hasActiveTradeForSymbol("EURUSD"));
    CHECK(manager.hasActiveTradeForSymbol("AUSIDXAUD"));
}

// openTrade does not enforce same-symbol uniqueness, so callers rely on
// hasActiveTradeForSymbol to gate same-symbol re-entry. This pins the invariant
// that a single open is enough to flip the helper to true.
TEST_CASE("Helper reports symbol active after first open", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick, 1, Direction::LONG);
    CHECK(manager.hasActiveTradeForSymbol("EURUSD"));
}

// --- Entry-side bid/ask handling ---

// A LONG buys at the ask, but its stop/limit must be measured from the bid (the
// price it would exit at), so entryPrice == ask, exitReferencePrice == bid.
// entryBid/entryAsk record the raw spread regardless of direction.
TEST_CASE("openTrade: LONG enters at ask and records the spread", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(tick, 1, Direction::LONG);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    CHECK(trade->second.entryPrice == 10000000);
    CHECK(trade->second.exitReferencePrice == 9900000);
    CHECK(trade->second.entryAsk == 10000000);
    CHECK(trade->second.entryBid == 9900000);
}

// Symmetric SHORT: sells at the bid, exit reference is the ask.
TEST_CASE("openTrade: SHORT enters at bid and records the spread", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(tick, 1, Direction::SHORT);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(tradeId);
    REQUIRE(trade != trades.end());

    CHECK(trade->second.entryPrice == 9900000);
    CHECK(trade->second.exitReferencePrice == 10000000);
    CHECK(trade->second.entryAsk == 10000000);
    CHECK(trade->second.entryBid == 9900000);
}

// --- Operations::run loop invariants ---

// Operations::run gates same-symbol re-entry on hasActiveTradeForSymbol and
// runs reviewStopAndLimit *before* the entry check on each tick, so a trade
// that stops out on a tick frees its symbol for re-entry on a later tick.
TEST_CASE("Run loop invariant: exit frees the symbol for re-entry", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string firstId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);
    REQUIRE(manager.hasActiveTradeForSymbol("EURUSD"));

    // Stop tick: bid drops 1 pip below the entry bid, so the LONG stops out.
    PriceData stopTick(110000, 109990, std::chrono::system_clock::now(), "EURUSD");
    trading::reviewStopAndLimit(manager, stopTick);

    CHECK_FALSE(manager.hasActiveTradeForSymbol("EURUSD"));
    const auto& closed = manager.getClosedTrades();
    REQUIRE(closed.size() == 1);
    CHECK(closed.front().closePrice == stopTick.bid);

    // The gate is open, so the loop would now allow a fresh entry on this symbol.
    std::string secondId = manager.openTrade(stopTick, 1, Direction::LONG, 1, 1);
    CHECK(firstId != secondId);
    CHECK(manager.hasActiveTradeForSymbol("EURUSD"));
}

// --- Operations::run smoke tests ---

// Operations::run owns its TradeManager internally and reports only to stdout
// and Elasticsearch, so there is no return value to assert against. These are
// deliberately smoke tests: with ELASTIC_ENABLED=0 the run must drive the full
// per-tick loop, summarise, and results path without throwing. RandomStrategy
// makes per-trade outcomes non-deterministic, so only the no-throw contract is
// checked. (Operations::run finishes by PUTting results to Elasticsearch; opt
// out so putTradingResults returns early — no network, no JSON serialisation.)

TEST_CASE("Operations::run handles an empty tick stream without throwing", "[tradeManager]") {
    setenv("ELASTIC_ENABLED", "0", 1);
    const std::vector<PriceData> ticks;
    const auto config = makeRandomStrategyConfig();
    CHECK_NOTHROW(Operations::run(ticks, config));
}

TEST_CASE("Operations::run processes a single tick without throwing", "[tradeManager]") {
    setenv("ELASTIC_ENABLED", "0", 1);
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, std::chrono::system_clock::now(), "EURUSD"),
    };
    const auto config = makeRandomStrategyConfig();
    CHECK_NOTHROW(Operations::run(ticks, config));
}

TEST_CASE("Operations::run processes a multi-tick stream without throwing", "[tradeManager]") {
    setenv("ELASTIC_ENABLED", "0", 1);
    // A drifting EURUSD series so the 10-pip SL/TP can actually fire across the
    // run, exercising both the entry and the reviewStopAndLimit exit paths.
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(110110, 110100, now, "EURUSD"),
        PriceData(110210, 110200, now, "EURUSD"),
        PriceData(110060, 110050, now, "EURUSD"),
        PriceData(109910, 109900, now, "EURUSD"),
    };
    const auto config = makeRandomStrategyConfig();
    CHECK_NOTHROW(Operations::run(ticks, config));
}

// --- Operations run loop (deterministic, end-to-end) ---

// These drive trading::runTicks — the exact per-tick loop Operations::run
// executes — with a deterministic injected strategy and a TradeManager we own,
// so trade outcomes (entry side, exit side, realised PnL) can be asserted
// directly. Units: EURUSD has 10 stored price-points per pip.

// LONG entry executes at the ask; the stop/limit reference is the bid.
TEST_CASE("runTicks: LONG opens at the ask", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(10, 10, 1);
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, std::chrono::system_clock::now(), "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    REQUIRE(tm.getActiveTrades().size() == 1);
    CHECK(tm.getClosedTrades().size() == 0);
    const Trade& trade = tm.getActiveTrades().begin()->second;
    CHECK(trade.direction == Direction::LONG);
    CHECK(trade.entryPrice == 110010);
    CHECK(trade.exitReferencePrice == 110000);
    CHECK(trade.entryAsk == 110010);
    CHECK(trade.entryBid == 110000);
}

// Symmetric SHORT: executes at the bid, exit reference is the ask.
TEST_CASE("runTicks: SHORT opens at the bid", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::SHORT});
    const auto vars = makeVars(10, 10, 1);
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, std::chrono::system_clock::now(), "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    REQUIRE(tm.getActiveTrades().size() == 1);
    const Trade& trade = tm.getActiveTrades().begin()->second;
    CHECK(trade.direction == Direction::SHORT);
    CHECK(trade.entryPrice == 110000);
    CHECK(trade.exitReferencePrice == 110010);
    CHECK(trade.entryAsk == 110010);
    CHECK(trade.entryBid == 110000);
}

// No signal -> no position, across many ticks.
TEST_CASE("runTicks: no signal opens nothing", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({});  // empty script: decide() always returns nullopt
    const auto vars = makeVars(10, 10, 1);
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(110110, 110100, now, "EURUSD"),
        PriceData(110210, 110200, now, "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 0);
    CHECK(tm.getClosedTrades().size() == 0);
}

// Re-entry is gated while a position is open: an always-signalling strategy on
// flat ticks (price never reaches the 10-pip SL/TP) must still open only one
// trade for the symbol, not one per tick.
TEST_CASE("runTicks: re-entry gated while active", "[tradeManager]") {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars(10, 10, 1);
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(110010, 110000, now, "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 1);
    CHECK(tm.getClosedTrades().size() == 0);
}

// LONG take-profit: a 10-pip favourable move nets only 90 points (9 pips)
// because entry was at the ask (110010) while the TP is measured from the entry
// bid (110000) and closes at the bid.
TEST_CASE("runTicks: LONG TP closes at bid, PnL net of spread", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});       // opens once, no re-entry
    const auto vars = makeVars(0, 10, 1);   // TP only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // open LONG @ ask 110010
        PriceData(110110, 110100, now, "EURUSD"),   // bid 110100 hits TP (ref 110000 + 100p)
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.closePrice == 110100);
    CHECK(closed.pnl == 90);
}

// Symmetric SHORT take-profit: enters at the bid (110000), TP measured from the
// entry ask (110010), closes at the ask. Again 90 points net of the spread.
TEST_CASE("runTicks: SHORT TP closes at ask, PnL net of spread", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::SHORT});
    const auto vars = makeVars(0, 10, 1);   // TP only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // open SHORT @ bid 110000
        PriceData(109910, 109900, now, "EURUSD"),   // ask 109910 hits TP (ref 110010 - 100p)
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.closePrice == 109910);
    CHECK(closed.pnl == 90);
}

// LONG stop-loss: a 10-pip adverse move loses 110 points (11 pips) because
// entry was at the ask 110010. Loss includes the spread.
TEST_CASE("runTicks: LONG SL closes at bid, negative PnL", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(10, 0, 1);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // open LONG @ ask 110010
        PriceData(109910, 109900, now, "EURUSD"),   // bid 109900 hits SL (ref 110000 - 100p)
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.closePrice == 109900);
    CHECK(closed.pnl == -110);
}

// Exit-before-entry ordering within a single tick: on the tick that stops the
// first LONG out, reviewStopAndLimit closes it first, the symbol frees, and the
// always-LONG strategy immediately re-enters on that same tick.
TEST_CASE("runTicks: exit then same-tick re-entry", "[tradeManager]") {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars(10, 0, 1);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // LONG#1 @ ask 110010
        PriceData(109910, 109900, now, "EURUSD"),   // stops LONG#1, re-opens LONG#2
    };

    trading::runTicks(tm, strategy, ticks, vars);

    REQUIRE(tm.getClosedTrades().size() == 1);
    REQUIRE(tm.getActiveTrades().size() == 1);

    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.entryPrice == 110010);
    CHECK(closed.closePrice == 109900);

    const Trade& reentry = tm.getActiveTrades().begin()->second;
    CHECK(reentry.direction == Direction::LONG);
    CHECK(reentry.entryPrice == 109910);
    CHECK(reentry.exitReferencePrice == 109900);
}

// Two symbols open simultaneously, each entering on the correct side of its own
// spread regardless of price scale (EURUSD ~110000 vs AUSIDXAUD ~700000).
TEST_CASE("runTicks: multi-symbol", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no exits — both persist
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(700050, 700000, now, "AUSIDXAUD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    REQUIRE(tm.getActiveTrades().size() == 2);
    const Trade* eur = nullptr;
    const Trade* aus = nullptr;
    for (const auto& [id, trade] : tm.getActiveTrades()) {
        if (trade.symbol == "EURUSD") eur = &trade;
        else if (trade.symbol == "AUSIDXAUD") aus = &trade;
    }
    REQUIRE(eur != nullptr);
    REQUIRE(aus != nullptr);
    CHECK(eur->entryPrice == 110010);
    CHECK(aus->entryPrice == 700050);
}

// --- Account loss limit (fail fast) ---

// FLOATING drawdown alone must trigger the cutoff: no stop-loss, so the open
// LONG's mark-to-market loss is the only thing the limit can see. The crash
// tick marks the trade at -1010 points; the floor is 10000 * 1% = 100 pips =
// 1000 points (pointsPerPip 10), so it breaches and the trade is liquidated.
TEST_CASE("runTicks: floating drawdown breach liquidates at mark", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no SL/TP at all
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // open LONG @ ask 110010
        PriceData(109010, 109000, now, "EURUSD"),   // mark at bid: floating -1010
        PriceData(109010, 109000, now, "EURUSD"),   // must never be processed
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "1"_dd,
                                     .pointsPerPip = 10};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::LossLimitBreached);
    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.closePrice == 109000);
    CHECK(closed.pnl == -1010);
    CHECK(closed.liquidated);
    CHECK(closed.floatingPnl == 0);
    CHECK(tm.calculatePnl() == -1010);
    CHECK(tm.unrealizedPnl() == 0);
}

// On breach, EVERY open trade is liquidated — each at its own symbol's last
// marked price. EURUSD crash (-1010) breaches the 1000-point floor; AUSIDXAUD,
// marked only at its entry tick, closes at its own mark for the -50 spread cost.
TEST_CASE("runTicks: liquidation closes every symbol at its own mark", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no SL/TP at all
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),     // open EURUSD LONG
        PriceData(700050, 700000, now, "AUSIDXAUD"),  // open AUSIDXAUD LONG
        PriceData(109010, 109000, now, "EURUSD"),     // EURUSD -1010: breach
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "1"_dd,
                                     .pointsPerPip = 10};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::LossLimitBreached);
    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 2);
    CHECK(tm.calculatePnl() == -1060);
    for (const Trade& closed : tm.getClosedTrades()) {
        CHECK(closed.liquidated);
        if (closed.symbol == "EURUSD") {
            CHECK(closed.closePrice == 109000);
            CHECK(closed.pnl == -1010);
        } else {
            CHECK(closed.closePrice == 700000);
            CHECK(closed.pnl == -50);
        }
    }
}

// MAX_OPEN_TRADES caps simultaneous positions across the whole run: with a
// cap of 1, the second symbol's signal is skipped while the first is open.
TEST_CASE("runTicks: max-open-trades cap blocks the second entry", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no exits — first stays open
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(700050, 700000, now, "AUSIDXAUD"),
    };
    const trading::RiskLimits limits{.maxOpenTrades = 1};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::Completed);
    REQUIRE(tm.getActiveTrades().size() == 1);
    CHECK(tm.getActiveTrades().begin()->second.symbol == std::string("EURUSD"));
}

// A stop-out that breaches the loss limit must end the run on that tick, BEFORE
// the entry phase — so the always-LONG strategy gets no same-tick re-entry, and
// later ticks never run. Floor: 10000 * 0.1% = 10 pips = 100 points; the single
// stop-out loses 110 points (incl. spread), so it breaches.
TEST_CASE("runTicks: loss-limit breach stops run before re-entry", "[tradeManager]") {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars(10, 0, 1);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // LONG#1 @ ask 110010
        PriceData(109910, 109900, now, "EURUSD"),   // stops LONG#1: pnl -110, breach
        PriceData(109910, 109900, now, "EURUSD"),   // must never be processed
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "0.1"_dd,
                                     .pointsPerPip = 10};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::LossLimitBreached);
    REQUIRE(tm.getClosedTrades().size() == 1);
    CHECK_FALSE(tm.getClosedTrades().front().liquidated);
    CHECK(tm.getActiveTrades().size() == 0);
    CHECK(tm.calculatePnl() == -110);
}

// A realized loss inside the limit must not stop the run: same stop-out, but a
// 5% limit (floor 5000 points) comfortably absorbs the -110-point loss, so the
// run completes and the same-tick re-entry happens.
TEST_CASE("runTicks: loss within limit runs to completion", "[tradeManager]") {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars(10, 0, 1);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(109910, 109900, now, "EURUSD"),   // stop-out -110, within -500
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "5"_dd,
                                     .pointsPerPip = 10};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::Completed);
    CHECK(tm.getClosedTrades().size() == 1);
    CHECK(tm.getActiveTrades().size() == 1);
}

// maxLossPercent <= 0 disables the check entirely — losses far past any
// percentage are ignored and the run completes. Cover both 0 and -1 spellings.
TEST_CASE("runTicks: loss limit disabled for zero and negative", "[tradeManager]") {
    const auto vars = makeVars(10, 0, 1);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(109910, 109900, now, "EURUSD"),   // stop-out -110
    };

    for (const auto percent : {"0"_dd, -"1"_dd}) {
        TradeManager tm;
        AlwaysLongStrategy strategy;
        // Tiny balance: -110 realized is over 100% of the account, yet with the
        // limit disabled the run must still complete.
        const trading::RiskLimits limits{.startingBalance = "10"_dd,
                                         .maxLossPercent = percent};

        const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

        CHECK(status == trading::RunStatus::Completed);
        CHECK(tm.getActiveTrades().size() == 1);
    }
}
