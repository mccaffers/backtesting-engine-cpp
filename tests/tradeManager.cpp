// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdint>
#include <cstdlib>            // setenv — disable Elastic reporting for run() smoke tests
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/decimal/literals.hpp>

#include "shared/tradingDefinitions/config/configuration.hpp"
#include "run/reporting/tradingResults.hpp"  // TradingResultsStats

import tradeManager;        // TradeManager
import resultsSummary;      // ResultsSummary::collect (performance score)
import exitRules;           // trading::exit_rules::checkExit
import reviewStopAndLimit;  // trading::reviewStopAndLimit
import runLoop;             // trading::runTicks, RiskLimits, RunStatus
import operations;          // Operations::run
import strategy;            // IStrategy
import barStore;            // bars::BarStore — decide() interface + gate tests
import entryConditions;     // conditions gate for the runTicks ATR tests
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
    vars.STOP_DISTANCE_IN_ATR = 10;
    vars.LIMIT_DISTANCE_IN_ATR = 10;
    vars.TRADING_SIZE = 1;
    return config;
}

// Per-test TradingVariables so each runTicks case can dial SL/TP independently
// (e.g. stop=0 to isolate the take-profit path). STRATEGY is unused by runTicks.
// With the ATR gate off (these tests don't pass one), runTicks forwards the
// values to openTrade as literal pip distances, so the scripted SL/TP geometry
// below stays exact.
tradingDefinitions::TradingVariables makeVars(int32_t stopPips,
                                               int32_t limitPips,
                                               int32_t size) {
    tradingDefinitions::TradingVariables vars;
    vars.STRATEGY = "Scripted";
    vars.STOP_DISTANCE_IN_ATR = stopPips;
    vars.LIMIT_DISTANCE_IN_ATR = limitPips;
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

    std::optional<Direction> decide(const PriceData& /*tick*/,
                                    const bars::BarStore& /*barStore*/) override {
        return index < script.size() ? script[index++] : std::nullopt;
    }
    void during(const PriceData& /*tick*/, const bars::BarStore& /*barStore*/,
                TradeManager& /*tradeManager*/) override {}
};

// Always signals LONG — used to probe re-entry behaviour (gating while a
// position is open; same-tick re-entry after a stop-out).
struct AlwaysLongStrategy : IStrategy {
    std::optional<Direction> decide(const PriceData& /*tick*/,
                                    const bars::BarStore& /*barStore*/) override {
        return Direction::LONG;
    }
    void during(const PriceData& /*tick*/, const bars::BarStore& /*barStore*/,
                TradeManager& /*tradeManager*/) override {}
};

// AlwaysLongStrategy that counts its calls — lets the peak-hours tests prove
// decide() was skipped on an out-of-session tick while during() still ran.
struct CountingLongStrategy : IStrategy {
    int decideCalls = 0;
    int duringCalls = 0;
    std::optional<Direction> decide(const PriceData& /*tick*/,
                                    const bars::BarStore& /*barStore*/) override {
        ++decideCalls;
        return Direction::LONG;
    }
    void during(const PriceData& /*tick*/, const bars::BarStore& /*barStore*/,
                TradeManager& /*tradeManager*/) override {
        ++duringCalls;
    }
};

// Seeds a closed EURUSD trade with an exact realized PnL. A zero-spread tick at
// 110000 means a LONG enters at 110000, so closing at 110000 + pnlPoints yields
// pnl == pnlPoints (size 1). EURUSD has 10 points per pip, so 10 points == 1 pip.
// Trades close in call order, which is the order collect() walks for drawdown.
void seedClosedTrade(TradeManager& manager, std::int32_t pnlPoints) {
    PriceData tick(110000, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick, 1, Direction::LONG);
    manager.closeTrade(tick.symbol, 110000 + pnlPoints, tick);
}

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
    manager.openTrade(tick, 1, Direction::LONG);
    bool closed = manager.closeTrade(tick.symbol, 11000000, tick);
    CHECK(closed);
    CHECK(manager.reviewAccount() == 0);
}

// One open trade per symbol is a TradeManager invariant now (activeTrades is
// keyed by symbol), so multiple concurrent trades means multiple symbols.
TEST_CASE("TradeManager tracks multiple trades across symbols", "[tradeManager]") {
    TradeManager manager;
    PriceData tick1(10000000, 9900000, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick2(20000000, 19900000, std::chrono::system_clock::now(), "GBPUSD");
    PriceData tick3(30000000, 29900000, std::chrono::system_clock::now(), "USDJPY");
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
    auto trade = trades.find(tick.symbol);

    REQUIRE(trade != trades.end());
    CHECK(trade->second.id == tradeId);
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
    auto trade = trades.find(entryTick.symbol);
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
    auto trade = trades.find(entryTick.symbol);
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
    auto trade = trades.find(entryTick.symbol);

    PriceData laterTick(110000, 109990, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, laterTick);
    REQUIRE(exit.has_value());
    CHECK(*exit == 109990);
}

// ENTRY_SLIPPAGE_TENTH_PIPS stress toggle: the haircut worsens what the entry
// PAID and nothing else. EURUSD is 10 points/pip, so 3 tenths = 3 points. The
// SL/TP anchors stay on the raw tick (exitReferencePrice), so a stressed and
// an unstressed run see identical market levels — only the PnL differs.
TEST_CASE("entry slippage worsens the LONG fill and leaves the anchors alone",
          "[tradeManager]") {
    TradeManager manager;
    manager.entrySlippageTenthPips = 3;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(entryTick, 1, Direction::LONG, 1, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(entryTick.symbol);
    REQUIRE(trade != trades.end());

    CHECK(trade->second.entryPrice == 110013);  // ask + 0.3 pip against us
    CHECK(trade->second.entryAsk == 110010);    // raw tick preserved (audit)
    CHECK(trade->second.entryBid == 110000);
    CHECK(trade->second.exitReferencePrice == 110000);   // anchor untouched
    CHECK(trade->second.stopPrice == 110000 - 10);       // same as unslipped
    CHECK(trade->second.limitPrice == 110000 + 10);
    // Open-tick equity dips by spread (10) + slippage (3).
    CHECK(trade->second.floatingPnl == -13);
    CHECK(manager.unrealizedPnl() == -13);
}

TEST_CASE("entry slippage worsens the SHORT fill symmetrically",
          "[tradeManager]") {
    TradeManager manager;
    manager.entrySlippageTenthPips = 3;
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(entryTick, 1, Direction::SHORT, 1, 1);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(entryTick.symbol);
    REQUIRE(trade != trades.end());

    CHECK(trade->second.entryPrice == 109997);  // bid - 0.3 pip against us
    CHECK(trade->second.exitReferencePrice == 110010);   // anchor = raw ask
    CHECK(trade->second.stopPrice == 110010 + 10);
    CHECK(trade->second.limitPrice == 110010 - 10);
    CHECK(trade->second.floatingPnl == -13);  // spread + slippage
}

// The realized cost of the stress is exactly slipPoints x size: same entry
// tick, same close price, PnL differs by the haircut alone.
TEST_CASE("entry slippage costs slipPoints times size on a closed round-trip",
          "[tradeManager]") {
    PriceData entryTick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    const std::int32_t closePrice = 110100;

    TradeManager plain;
    plain.openTrade(entryTick, 2, Direction::LONG);
    plain.closeTrade(entryTick.symbol, closePrice, entryTick);

    TradeManager stressed;
    stressed.entrySlippageTenthPips = 3;
    stressed.openTrade(entryTick, 2, Direction::LONG);
    stressed.closeTrade(entryTick.symbol, closePrice, entryTick);

    CHECK(plain.calculatePnl() == (110100 - 110010) * 2);
    CHECK(plain.calculatePnl() - stressed.calculatePnl() == 3 * 2);
}

// Tenth-pips convert through the SYMBOL's points-per-pip: XAUUSD is 1000
// points/pip, so the same 3-tenths setting is 300 points there, not 3.
TEST_CASE("entry slippage scales per symbol (metals: 3 tenths = 300 points)",
          "[tradeManager]") {
    TradeManager manager;
    manager.entrySlippageTenthPips = 3;
    PriceData entryTick(2400500, 2400000, std::chrono::system_clock::now(), "XAUUSD");
    manager.openTrade(entryTick, 1, Direction::LONG);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(entryTick.symbol);
    REQUIRE(trade != trades.end());
    CHECK(trade->second.entryPrice == 2400500 + 300);
    CHECK(trade->second.exitReferencePrice == 2400000);
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
    auto trade = trades.find(entryTick.symbol);
    REQUIRE(trade != trades.end());

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    CHECK_FALSE(exit.has_value());
}

TEST_CASE("checkExit: LONG SL fires at bid", "[tradeManager]") {
    TradeManager manager;
    PriceData entryTick(110011, 110001, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = manager.openTrade(entryTick, 1, Direction::LONG, 1, 0);
    auto trades = manager.getActiveTrades();
    auto trade = trades.find(entryTick.symbol);
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
    auto trade = trades.find(entryTick.symbol);
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
    auto trade = trades.find(entryTick.symbol);
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
    auto trade = trades.find(entryTick.symbol);
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
    CHECK(trades.find(entryTick.symbol) != trades.end());
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
    CHECK(active.find(entryTick.symbol) == active.end());

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
    CHECK(trades.find(entryTick.symbol) != trades.end());
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
    manager.openTrade(tick, 1, Direction::LONG);
    REQUIRE(manager.hasActiveTradeForSymbol("EURUSD"));

    bool closed = manager.closeTrade(tick.symbol, 110000, tick);
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

// activeTrades is keyed by symbol, so openTrade refuses a same-symbol
// double-open: the second call returns the existing trade's id and leaves the
// account untouched. Callers still gate re-entry on hasActiveTradeForSymbol;
// this pins the fallback behaviour if that gate is ever bypassed.
TEST_CASE("openTrade refuses a second open on an active symbol", "[tradeManager]") {
    TradeManager manager;
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    std::string firstId = manager.openTrade(tick, 1, Direction::LONG);
    CHECK(manager.hasActiveTradeForSymbol("EURUSD"));

    std::string secondId = manager.openTrade(tick, 1, Direction::SHORT);
    CHECK(secondId == firstId);
    CHECK(manager.reviewAccount() == 1);
    // The original LONG survives; the refused SHORT never entered the book.
    CHECK(manager.getActiveTrades().begin()->second.direction == Direction::LONG);
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
    auto trade = trades.find(tick.symbol);
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
    auto trade = trades.find(tick.symbol);
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

// LONG entry executes at the ask; the stop/limit reference is the bid. The run
// ends with the position still open, so end-of-data closes it at its last mark
// (the entry bid) — an ordinary close realizing the spread, not a liquidation.
TEST_CASE("runTicks: LONG opens at the ask", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(10, 10, 1);
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, std::chrono::system_clock::now(), "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& trade = tm.getClosedTrades().front();
    CHECK(trade.direction == Direction::LONG);
    CHECK(trade.entryPrice == 110010);
    CHECK(trade.exitReferencePrice == 110000);
    CHECK(trade.entryAsk == 110010);
    CHECK(trade.entryBid == 110000);
    CHECK(trade.closePrice == 110000);   // last mark: the entry bid
    CHECK(trade.pnl == -10);             // the spread
    CHECK_FALSE(trade.liquidated);
}

// Symmetric SHORT: executes at the bid, exit reference is the ask; end-of-data
// closes it at the entry ask for the spread.
TEST_CASE("runTicks: SHORT opens at the bid", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::SHORT});
    const auto vars = makeVars(10, 10, 1);
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, std::chrono::system_clock::now(), "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& trade = tm.getClosedTrades().front();
    CHECK(trade.direction == Direction::SHORT);
    CHECK(trade.entryPrice == 110000);
    CHECK(trade.exitReferencePrice == 110010);
    CHECK(trade.entryAsk == 110010);
    CHECK(trade.entryBid == 110000);
    CHECK(trade.closePrice == 110010);   // last mark: the entry ask
    CHECK(trade.pnl == -10);             // the spread
    CHECK_FALSE(trade.liquidated);
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
// trade for the symbol, not one per tick. The single position is then closed
// by end-of-data, so exactly one trade exists in total.
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

    CHECK(tm.getActiveTrades().size() == 0);
    CHECK(tm.getClosedTrades().size() == 1);
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

// Peak-hours filter: an out-of-session tick skips decide() entirely (never
// deferred) while during() still runs; the in-session tick enters as normal.
// EURUSD is a Europe symbol, and Wed 2026-07-15 is under BST, so the window
// is 07:00-10:00 UTC.
TEST_CASE("runTicks: peak-hours filter blocks out-of-session entries",
          "[tradeManager]") {
    TradeManager tm;
    CountingLongStrategy strategy;
    const auto vars = makeVars(0, 0, 1);   // no SL/TP — end-of-data closes
    const auto day = std::chrono::sys_days{std::chrono::year{2026} / 7 / 15};
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, day + std::chrono::hours{6}, "EURUSD"),
        PriceData(110020, 110010, day + std::chrono::hours{8}, "EURUSD"),
    };
    const trading::RiskLimits limits{.peakHoursOnly = true};

    trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(strategy.decideCalls == 1);   // 06:00 tick never reached decide()
    CHECK(strategy.duringCalls == 2);   // during() is never gated
    REQUIRE(tm.getClosedTrades().size() == 1);
    CHECK(tm.getClosedTrades().front().entryPrice == 110020);  // the 08:00 ask
}

// The filter gates ENTRIES only: a stop-loss still fires on an out-of-session
// tick. USDJPY is an Asia symbol (00:00-06:00 UTC), so the 12:00 tick is
// outside its window — yet it closes the trade opened at 01:00.
TEST_CASE("runTicks: peak-hours filter never gates exits", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(10, 0, 1);   // SL only
    const auto day = std::chrono::sys_days{std::chrono::year{2026} / 7 / 15};
    const std::vector<PriceData> ticks{
        // open LONG @ ask 110010, SL ref 110000 - 100 points
        PriceData(110010, 110000, day + std::chrono::hours{1}, "USDJPY"),
        // out-of-session, but bid 109900 hits the stop
        PriceData(109910, 109900, day + std::chrono::hours{12}, "USDJPY"),
    };
    const trading::RiskLimits limits{.peakHoursOnly = true};

    trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.closePrice == 109900);
    CHECK_FALSE(closed.liquidated);
}

// ATR entry conditions, cold store: with the gate wired but the gate series
// short of the 11 bars ATR(10) needs, every entry is skipped BEFORE decide()
// while during() still runs on every tick. Ticks a minute apart never roll a
// second 15m bar, so the store stays cold for the whole run.
TEST_CASE("runTicks: cold ATR gate skips decide() while during() still runs",
          "[tradeManager]") {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query
    TradeManager tm;
    CountingLongStrategy strategy;
    const auto vars = makeVars(1, 3, 1);  // ATR multipliers
    bars::BarStore store;
    const bars::SeriesSpec gate{std::chrono::minutes{15}, 11};
    store.registerSeries(gate.minutes, gate.count);

    const auto day = std::chrono::sys_days{std::chrono::year{2026} / 7 / 15};
    std::vector<PriceData> ticks;
    for (int i = 0; i < 5; ++i) {
        ticks.emplace_back(110000, 110000, day + std::chrono::minutes{i},
                           "EURUSD");
    }

    trading::runTicks(tm, strategy, ticks, vars, {}, &store, gate);

    CHECK(strategy.decideCalls == 0);   // gate failed pre-decide on every tick
    CHECK(strategy.duringCalls == 5);   // during() is never gated
    CHECK(tm.getActiveTrades().empty());
    CHECK(tm.getClosedTrades().empty());
}

// ATR entry conditions, warm store: zero-spread ticks 16 minutes apart each
// roll a fresh 15m bar and step the price 200 points (20 EURUSD pips), so
// every true range is 200 and ATR(10) reads exactly 200 points. The store
// updates BEFORE the gates, so the gate on tick i sees i+1 bars: tick 10 is
// the first warm one, and its entry carries the dynamic distances
// (stop = 20 pips x 1, limit = 20 pips x 3), not the raw multipliers.
TEST_CASE("runTicks: warm ATR gate opens with dynamic distances",
          "[tradeManager]") {
    setenv("OHLC_PREPOPULATE", "0", 1);
    TradeManager tm;
    CountingLongStrategy strategy;
    const auto vars = makeVars(1, 3, 1);  // ATR multipliers
    bars::BarStore store;
    const bars::SeriesSpec gate{std::chrono::minutes{15}, 11};
    store.registerSeries(gate.minutes, gate.count);

    const auto day = std::chrono::sys_days{std::chrono::year{2026} / 7 / 15};
    std::vector<PriceData> ticks;
    for (int i = 0; i < 12; ++i) {
        const std::int32_t price = 110000 + i * 200;
        ticks.emplace_back(price, price, day + std::chrono::minutes{16 * i},
                           "EURUSD");
    }

    trading::runTicks(tm, strategy, ticks, vars, {}, &store, gate);

    // Ticks 0-9 skipped pre-decide; tick 10 enters; tick 11 is gated by the
    // open position, so decide() ran exactly once.
    CHECK(strategy.decideCalls == 1);
    // The end-of-data close settles the position; the closed trade still
    // carries the ATR-derived distances the entry was opened with.
    CHECK(tm.getActiveTrades().empty());
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.entryPrice == 112000);  // tick 10's ask
    CHECK(closed.stopDistancePips == 20);
    CHECK(closed.limitDistancePips == 60);
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

    // Two trades in close order: the stop-out, then the same-tick re-entry
    // (closed by end-of-data at its last mark).
    REQUIRE(tm.getClosedTrades().size() == 2);
    CHECK(tm.getActiveTrades().size() == 0);

    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.entryPrice == 110010);
    CHECK(closed.closePrice == 109900);

    const Trade& reentry = tm.getClosedTrades().back();
    CHECK(reentry.direction == Direction::LONG);
    CHECK(reentry.entryPrice == 109910);
    CHECK(reentry.exitReferencePrice == 109900);
    CHECK_FALSE(reentry.liquidated);
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

    // Both positions persist to end-of-data, where each closes at its own
    // symbol's last mark.
    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 2);
    const Trade* eur = nullptr;
    const Trade* aus = nullptr;
    for (const auto& trade : tm.getClosedTrades()) {
        if (trade.symbol == "EURUSD") eur = &trade;
        else if (trade.symbol == "AUSIDXAUD") aus = &trade;
    }
    REQUIRE(eur != nullptr);
    REQUIRE(aus != nullptr);
    CHECK(eur->entryPrice == 110010);
    CHECK(aus->entryPrice == 700050);
    CHECK_FALSE(eur->liquidated);
    CHECK_FALSE(aus->liquidated);
}

// --- Account loss limit (fail fast) ---

// FLOATING drawdown alone must trigger the cutoff: no stop-loss, so the open
// LONG's mark-to-market loss is the only thing the limit can see. The crash
// tick marks the trade at -1010 points; the pip BUDGET (balance × percent read
// directly as pips — no pip-value model) is 10000 × 1% = 100 pips = 1000 points
// (pointsPerPip 10), so it breaches and the trade is liquidated.
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
    CHECK(tm.getActiveTrades().size() == 0);
    // Only the EURUSD entry got through the cap; end-of-data closed it.
    REQUIRE(tm.getClosedTrades().size() == 1);
    CHECK(tm.getClosedTrades().front().symbol == std::string("EURUSD"));
}

// MAX_TRADES_PER_MINUTE is a sliding window over TICK time: with a cap of 1,
// the AUSIDXAUD signal 30s after the EURUSD entry is skipped (the window still
// holds that entry), but exactly 60s after it the entry has aged out (the
// window is half-open) and AUSIDXAUD enters — at THAT tick's price, proving
// the capped attempt was skipped outright, not deferred. A skipped tick never
// reaches decide(), so it doesn't consume a scripted signal either.
TEST_CASE("runTicks: trade rate cap enforces a sliding one-minute window", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no exits — both persist to end
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),                               // opens
        PriceData(700050, 700000, now + std::chrono::seconds{30}, "AUSIDXAUD"), // capped
        PriceData(700150, 700100, now + std::chrono::seconds{60}, "AUSIDXAUD"), // opens
    };
    const trading::RiskLimits limits{.maxTradesPerMinute = 1};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::Completed);
    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 2);
    const Trade* aus = nullptr;
    for (const auto& trade : tm.getClosedTrades()) {
        if (trade.symbol == "AUSIDXAUD") aus = &trade;
    }
    REQUIRE(aus != nullptr);
    // Entered on the third tick, not the capped second one.
    CHECK(aus->entryPrice == 700150);
}

// A same-instant burst counts against the cap too: three signals on one
// timestamp with a cap of 2 opens exactly two trades (the window can never
// slide within a single tick's timestamp).
TEST_CASE("runTicks: trade rate cap blocks a same-instant burst", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG, Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no exits
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(700050, 700000, now, "AUSIDXAUD"),
        PriceData(130010, 130000, now, "GBPUSD"),   // capped
    };
    const trading::RiskLimits limits{.maxTradesPerMinute = 2};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::Completed);
    REQUIRE(tm.getClosedTrades().size() == 2);
    for (const auto& trade : tm.getClosedTrades()) {
        CHECK(trade.symbol != std::string("GBPUSD"));
    }
}

// A stop-out that breaches the loss limit must end the run on that tick, BEFORE
// the entry phase — so the always-LONG strategy gets no same-tick re-entry, and
// later ticks never run. Pip budget: 10000 × 0.1% = 10 pips = 100 points; the
// single stop-out loses 110 points (incl. spread), so it breaches.
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
// 5% budget (500 pips = 5000 points) comfortably absorbs the -110-point loss,
// so the run completes and the same-tick re-entry happens (and is then closed
// by end-of-data as an ordinary, non-liquidated close).
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
    REQUIRE(tm.getClosedTrades().size() == 2);   // stop-out + end-of-data close
    CHECK(tm.getActiveTrades().size() == 0);
    CHECK_FALSE(tm.getClosedTrades().back().liquidated);
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
        // Stop-out plus the end-of-data close of the same-tick re-entry.
        CHECK(tm.getClosedTrades().size() == 2);
        CHECK(tm.getActiveTrades().size() == 0);
    }
}

// --- runTicks performance gate ---

// The thresholds below are deliberately test-local: they exercise the gate
// machinery (strict more-than comparisons, each check independently
// disableable, the Underperformed status) — not the production values
// Operations configures.

namespace {

// Deterministic two-winner fixture for the gate tests: two TP exits (+90 each,
// see the TP cases above for the unit conventions) with zero drawdown, giving
// exactly 2 decisive trades and a modest positive performance score.
trading::RunStatus runTwoWinnerFixture(const trading::RiskLimits& limits) {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars(0, 10, 1);   // TP only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),  // LONG#1 @ ask 110010
        PriceData(110110, 110100, now, "EURUSD"),  // TP#1; re-enter @ 110110
        PriceData(110210, 110200, now, "EURUSD"),  // TP#2; script exhausted
    };
    return trading::runTicks(tm, strategy, ticks, vars, limits);
}

}  // namespace

TEST_CASE("runTicks: performance gate off by default", "[tradeManager]") {
    CHECK(runTwoWinnerFixture({}) == trading::RunStatus::Completed);
}

// The floor is a strict more-than on decisive (winner/loser) trades: with 2
// decisive trades a floor of 1 passes, a floor of 2 does not.
TEST_CASE("runTicks: decisive-trade floor gates completion", "[tradeManager]") {
    CHECK(runTwoWinnerFixture({.minDecisiveTrades = 1})
          == trading::RunStatus::Completed);
    CHECK(runTwoWinnerFixture({.minDecisiveTrades = 2})
          == trading::RunStatus::Underperformed);
}

// The two-winner fixture scores comfortably above 1 and nowhere near 1000
// (zero drawdown, positive expectancy); lastMonths supplies the annualisation
// horizon the score needs whenever the score threshold is active.
TEST_CASE("runTicks: performance-score threshold gates completion", "[tradeManager]") {
    CHECK(runTwoWinnerFixture({.minPerformanceScore = "1"_dd, .lastMonths = 1})
          == trading::RunStatus::Completed);
    CHECK(runTwoWinnerFixture({.minPerformanceScore = "1000"_dd, .lastMonths = 1})
          == trading::RunStatus::Underperformed);
}

// End-of-data close realizes the CURRENT mark, not the entry: the open LONG has
// floated +90 (bid 110100 vs entry ask 110010) when the data runs out, so the
// forced close books +90 and the account carries no dangling floating PnL into
// reporting (finalPnl and max drawdown now describe the same equity curve).
TEST_CASE("runTicks: end of data closes open trades at their last mark", "[tradeManager]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(0, 0, 1);    // no SL/TP: only end-of-data can close
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // open LONG @ ask 110010
        PriceData(110110, 110100, now, "EURUSD"),   // mark at bid 110100: +90
    };

    const auto status = trading::runTicks(tm, strategy, ticks, vars);

    CHECK(status == trading::RunStatus::Completed);
    CHECK(tm.getActiveTrades().size() == 0);
    REQUIRE(tm.getClosedTrades().size() == 1);
    const Trade& closed = tm.getClosedTrades().front();
    CHECK(closed.closePrice == 110100);
    CHECK(closed.pnl == 90);
    CHECK_FALSE(closed.liquidated);
    CHECK(tm.calculatePnl() == 90);
    CHECK(tm.unrealizedPnl() == 0);
}

// The loss floor is a budget on PRICE MOVEMENT, so it scales with trade size:
// PnL is tracked in points × size, and an unscaled floor would silently shrink
// to budget/size pips. Size 2 with a 15-pip budget (150 points, scaled floor
// -300): the stop-out books -110 points × 2 = -220, inside the scaled floor —
// an unscaled floor (-150) would have (wrongly) ended this run.
TEST_CASE("runTicks: loss floor scales with trade size", "[tradeManager]") {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars(10, 0, 2);   // SL only, size 2
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),
        PriceData(109910, 109900, now, "EURUSD"),   // stop-out: -110 points × 2
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "0.15"_dd,
                                     .pointsPerPip = 10};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    CHECK(status == trading::RunStatus::Completed);
    CHECK(tm.calculatePnl() == -240);   // -220 stop-out, -20 re-entry spread ×2
}

// --- Performance score (ResultsSummary::collect) ---

// Guard: with no closed trades there is nothing to score, so every score field
// stays at its 0 default rather than producing a NaN from a 0/0 division.
TEST_CASE("score: no trades leaves score fields zero", "[score]") {
    TradeManager tm;
    const auto config = makeRandomStrategyConfig();   // LAST_MONTHS = 1, balance 10000

    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(static_cast<double>(stats.performanceScore) == 0.0);
    CHECK(static_cast<double>(stats.expectancyScore) == 0.0);
    CHECK(static_cast<double>(stats.calmarScore) == 0.0);
    CHECK(static_cast<double>(stats.maxDrawdownPercent) == 0.0);
}

// Guard: a zero-length horizon (LAST_MONTHS = 0) would divide by zero in the
// CAGR/confidence maths, so the score is suppressed even with real trades.
TEST_CASE("score: zero horizon suppresses the score", "[score]") {
    TradeManager tm;
    seedClosedTrade(tm, 100);   // a +10-pip winner
    auto config = makeRandomStrategyConfig();
    config.LAST_MONTHS = 0;

    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(stats.winners == 1);
    CHECK(static_cast<double>(stats.performanceScore) == 0.0);
}

// All-winners special cases mirror the C# reference: no losers -> win rate 1 and
// trade ratio pinned to 100, and the run still produces a positive score.
TEST_CASE("score: all winners pin winRate=1 and tradeRatio=100", "[score]") {
    TradeManager tm;
    seedClosedTrade(tm, 100);   // +10 pips
    seedClosedTrade(tm, 100);   // +10 pips
    const auto config = makeRandomStrategyConfig();

    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(stats.winners == 2);
    CHECK(stats.losers == 0);
    CHECK(static_cast<double>(stats.winRate) == 1.0);
    CHECK(static_cast<double>(stats.tradeRatio) == 100.0);
    CHECK(static_cast<double>(stats.maxDrawdownPercent) == 0.0);  // monotonic up
    CHECK(static_cast<double>(stats.performanceScore) > 0.0);
}

// winRate is the share of ALL closed trades that won — breakevens count in the
// denominator, so one winner among three breakevens is 25%, not a forced 100%
// (previously a run with zero losers pinned winRate to 1.0 no matter how many
// breakevens it had).
TEST_CASE("score: breakevens dilute winRate", "[score]") {
    TradeManager tm;
    seedClosedTrade(tm, 100);   // +10 pips
    seedClosedTrade(tm, 0);     // breakeven
    seedClosedTrade(tm, 0);     // breakeven
    seedClosedTrade(tm, 0);     // breakeven
    const auto config = makeRandomStrategyConfig();

    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(stats.winners == 1);
    CHECK(stats.breakeven == 3);
    CHECK(static_cast<double>(stats.winRate) == Catch::Approx(0.25));
    // Still no losers, so tradeRatio stays pinned at the cap.
    CHECK(static_cast<double>(stats.tradeRatio) == 100.0);
}

// A closed trade on a symbol with no known scale (scalingFactor == 0) cannot be
// expressed in pips: it is excluded from winners/losers/breakeven and the pip
// sums alike — counting it as a winner that contributes zero pips would skew
// the averages (and could zero averageLoss into a divide-by-Inf tradeRatio).
TEST_CASE("score: unknown-symbol trades are excluded from pip metrics", "[score]") {
    TradeManager tm;
    seedClosedTrade(tm, 100);   // +10 pips on EURUSD (known scale)
    PriceData tick(500, 500, std::chrono::system_clock::now(), "ZZZTEST");
    tm.openTrade(tick, 1, Direction::LONG);
    tm.closeTrade("ZZZTEST", 600, tick);   // +100 points on an unknown scale
    const auto config = makeRandomStrategyConfig();

    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(stats.tradesClosed == 2);
    CHECK(stats.winners == 1);   // only the EURUSD winner is measurable
    CHECK(stats.losers == 0);
    CHECK(stats.breakeven == 0);
    CHECK(static_cast<double>(stats.finalPnl) == Catch::Approx(10.0));
}

// Mixed run pins the derived stats to hand-computable values. PnL sequence in
// pips: +10, -4, -3, +2 -> cumulative 10, 6, 3, 5, so the realized peak-to-trough
// is 10 - 3 = 7 pips. Against a 10000 balance that is 0.07%. winRate = 2/4 = 0.5;
// averageWin = 6, averageLoss = 3.5, tradeRatio = (6*2)/(3.5*2) = 12/7.
TEST_CASE("score: drawdown and ratios match a hand-computed run", "[score]") {
    TradeManager tm;
    seedClosedTrade(tm, 100);    // +10 pips
    seedClosedTrade(tm, -40);    //  -4 pips
    seedClosedTrade(tm, -30);    //  -3 pips
    seedClosedTrade(tm, 20);     //  +2 pips
    const auto config = makeRandomStrategyConfig();

    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(stats.winners == 2);
    CHECK(stats.losers == 2);
    CHECK(static_cast<double>(stats.winRate) == Catch::Approx(0.5));
    CHECK(static_cast<double>(stats.tradeRatio) == Catch::Approx(12.0 / 7.0));
    CHECK(static_cast<double>(stats.maxDrawdownPercent) == Catch::Approx(0.07));
}

// True intra-trade drawdown: a single LONG that floats deep underwater and then
// recovers to a winning close. Close-to-close (realized) drawdown would be 0
// because the only closed trade is a winner — the mark-to-market tracker must
// still see the trough the open position sat through. Open @ ask 110010, mark
// down to bid 109000 (floating -1010 points = -101 pips), then TP-close at +90.
// Scripted (single-shot) so no same-tick re-entry muddies the trade count.
TEST_CASE("score: drawdown captures an intra-trade float, not just closes", "[score]") {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars(0, 10, 1);   // TP only, no SL — let it float
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData(110010, 110000, now, "EURUSD"),   // open LONG @ ask 110010
        PriceData(109010, 109000, now, "EURUSD"),   // floats to -1010 points
        PriceData(110110, 110100, now, "EURUSD"),   // bid 110100 hits TP, closes +90
    };

    trading::runTicks(tm, strategy, ticks, vars);

    REQUIRE(tm.getClosedTrades().size() == 1);
    CHECK(tm.getClosedTrades().front().pnl == 90);   // the only close is a winner

    const auto config = makeRandomStrategyConfig();
    const auto stats = ResultsSummary::collect(tm, config);

    CHECK(stats.winners == 1);
    CHECK(stats.losers == 0);
    // Trough was -1010 points = -101 pips from the 0 peak; 101 / 10000 * 100.
    CHECK(static_cast<double>(stats.maxDrawdownPercent) == Catch::Approx(1.01));
}
