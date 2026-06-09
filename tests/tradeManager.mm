// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import <stdlib.h>            // setenv — disable Elastic reporting for run() smoke tests
#import <optional>
#import <utility>
#import <vector>
#import <boost/decimal/literals.hpp>
#import "tradeManager.hpp"
#import "exitRules.hpp"
#import "reviewStopAndLimit.hpp"
#import "runLoop.hpp"
#import "operations.hpp"
#import "strategies/strategy.hpp"
#import "trading_definitions/configuration.hpp"

// Pulls in the _dd user-defined literal so "1.23"_dd produces a decimal64_t
// directly. decimal64_t has no implicit conversion from double — the closest
// C# analogue is having to write `1.23m` instead of `1.23` for a decimal.
using namespace boost::decimal::literals;

namespace {

// Minimal Configuration that drives Operations::run down the RandomStrategy
// path. selectStrategy only reads STRATEGY.TRADING_VARIABLES.STRATEGY, and
// RandomStrategy ignores everything else in the strategy block, so the OHLC
// and strategy-variable sections are left default-constructed. The pip / size
// values are plausible but irrelevant to the no-throw assertions below.
trading_definitions::Configuration makeRandomStrategyConfig() {
    trading_definitions::Configuration config;
    config.RUN_ID = "TEST_RUN";
    config.SYMBOLS = "EURUSD";
    config.LAST_MONTHS = 1;
    config.STRATEGY.UUID = "test-strategy-uuid";
    auto& vars = config.STRATEGY.TRADING_VARIABLES;
    vars.STRATEGY = "RandomStrategy";
    vars.STOP_DISTANCE_IN_PIPS = "10"_dd;
    vars.LIMIT_DISTANCE_IN_PIPS = "10"_dd;
    vars.TRADING_SIZE = "1.0"_dd;
    return config;
}

// Per-test TradingVariables so each runTicks case can dial SL/TP independently
// (e.g. stop=0 to isolate the take-profit path). STRATEGY is unused by runTicks.
trading_definitions::TradingVariables makeVars(boost::decimal::decimal64_t stopPips,
                                               boost::decimal::decimal64_t limitPips,
                                               boost::decimal::decimal64_t size) {
    trading_definitions::TradingVariables vars;
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

@interface TradeManagerTests : XCTestCase
@property (nonatomic) TradeManager* manager;
@end

@implementation TradeManagerTests

- (void)setUp {
    self.manager = new TradeManager();
    // Operations::run finishes by PUTting results to Elasticsearch. There is no
    // Elastic instance under test, so opt out: putTradingResults returns early
    // (no network, no JSON serialisation) when ELASTIC_ENABLED=0.
    setenv("ELASTIC_ENABLED", "0", 1);
}

- (void)tearDown {
    delete self.manager;
    self.manager = nullptr;
}

- (void)testOpenTrade {
    PriceData tick("100.0"_dd, "99.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);
    XCTAssertFalse(tradeId.empty(), "Trade ID should not be empty");
    XCTAssertEqual(self.manager->reviewAccount(), 1, "Should have 1 active trade");
}

- (void)testCloseTrade {
    PriceData tick("100.0"_dd, "99.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);
    bool closed = self.manager->closeTrade(tradeId, "110.0"_dd, tick);
    XCTAssertTrue(closed, "Trade should be closed successfully");
    XCTAssertEqual(self.manager->reviewAccount(), 0, "Should have 0 active trades");
}

- (void)testMultipleTrades {
    PriceData tick1("100.0"_dd, "99.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick2("200.0"_dd, "199.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick3("300.0"_dd, "299.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    self.manager->openTrade(tick1, "1.0"_dd, Direction::LONG);
    self.manager->openTrade(tick2, "2.0"_dd, Direction::SHORT);
    self.manager->openTrade(tick3, "3.0"_dd, Direction::LONG);

    XCTAssertEqual(self.manager->reviewAccount(), 3, "Should have 3 active trades");
}

- (void)testTradeDetails {
    PriceData tick("100.0"_dd, "99.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);

    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");
    XCTAssertEqual(trade->second.entryPrice, "100.0"_dd, "Entry price should match");
    XCTAssertEqual(trade->second.size, "1.0"_dd, "Size should match");
    XCTAssertTrue(trade->second.direction == Direction::LONG, "Trade should be long");
}

// Regression: with a 1-pip EURUSD spread and a 1-pip stop, a LONG must not
// be stopped out on its own opening tick. The stop is measured from the
// adverse side (entry bid), not the execution price (entry ask) — otherwise
// the spread alone trips the stop.
- (void)testLongDoesNotExitOnEntryTickWithOnePipSpread {
    PriceData entryTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");
    XCTAssertEqual(trade->second.entryPrice, "1.1001"_dd, "LONG entry price should be ask");
    XCTAssertEqual(trade->second.exitReferencePrice, "1.1000"_dd,
                   "LONG exit reference should be entry bid");

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    XCTAssertFalse(exit.has_value(),
                   "LONG must not exit on its own entry tick when spread == stop distance");
}

// Symmetric SHORT case: must not be stopped out on the opening tick.
- (void)testShortDoesNotExitOnEntryTickWithOnePipSpread {
    PriceData entryTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::SHORT,
                                                  "1"_dd, "1"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");
    XCTAssertEqual(trade->second.entryPrice, "1.1000"_dd, "SHORT entry price should be bid");
    XCTAssertEqual(trade->second.exitReferencePrice, "1.1001"_dd,
                   "SHORT exit reference should be entry ask");

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    XCTAssertFalse(exit.has_value(),
                   "SHORT must not exit on its own entry tick when spread == stop distance");
}

// Sanity check: once price moves enough, the stop still fires — the fix
// shifts the threshold by one pip, it does not disable exits.
- (void)testLongStopsOutWhenBidDropsBelowEntryBidByOnePip {
    PriceData entryTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);

    PriceData laterTick("1.1000"_dd, "1.0999"_dd, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, laterTick);
    XCTAssertTrue(exit.has_value(), "LONG should stop when bid falls 1 pip below entry bid");
    XCTAssertEqual(*exit, "1.0999"_dd, "Stop closes at the current bid");
}

// LONG × SL/TP × flat tick: passing the entry tick itself back through
// checkExit must not fire either side. This pins the spread-vs-stop
// invariant: SL is anchored on the entry bid (exitReferencePrice), not
// the execution ask, so a 1-pip spread with a 1-pip stop sits exactly
// at the threshold without crossing it.
- (void)testCheckExit_LongFlatTick_NoFire {
    PriceData entryTick("1.10011"_dd, "1.10001"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    auto exit = trading::exit_rules::checkExit(trade->second, entryTick);
    XCTAssertFalse(exit.has_value(),
                   "LONG with 1-pip SL and TP must not fire on its own entry tick");
}

- (void)testCheckExit_LongSL_FiresAtBid {
    PriceData entryTick("1.10011"_dd, "1.10001"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "0"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    // 1 pip on EURUSD == 0.0001 (the 4th decimal), so entry-bid 1.10001 minus
    // 1 pip is 1.09991; the 5th decimal is a fractional pip and not enough
    // to trip a 1-pip stop on its own.
    PriceData nextTick("1.10001"_dd, "1.09991"_dd, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    XCTAssertTrue(exit.has_value(), "LONG SL should fire when bid hits entry-bid - 1 pip");
    XCTAssertEqual(*exit, nextTick.bid, "LONG SL closes at the current bid");
    XCTAssertEqual(*exit, "1.09991"_dd, "LONG SL close price should equal next-tick bid");
}

- (void)testCheckExit_ShortSL_FiresAtAsk {
    PriceData entryTick("1.10011"_dd, "1.10001"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::SHORT,
                                                  "1"_dd, "0"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    PriceData nextTick("1.10021"_dd, "1.10011"_dd, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    XCTAssertTrue(exit.has_value(), "SHORT SL should fire when ask hits entry-ask + 1 pip");
    XCTAssertEqual(*exit, nextTick.ask, "SHORT SL closes at the current ask");
    XCTAssertEqual(*exit, "1.10021"_dd, "SHORT SL close price should equal next-tick ask");
}

- (void)testCheckExit_LongTP_FiresAtBid {
    PriceData entryTick("1.10011"_dd, "1.10001"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "0"_dd, "1"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    PriceData nextTick("1.10021"_dd, "1.10011"_dd, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    XCTAssertTrue(exit.has_value(), "LONG TP should fire when bid hits entry-bid + 1 pip");
    XCTAssertEqual(*exit, nextTick.bid, "LONG TP closes at the current bid");
    XCTAssertEqual(*exit, "1.10011"_dd, "LONG TP close price should equal next-tick bid");
}

- (void)testCheckExit_ShortTP_FiresAtAsk {
    PriceData entryTick("1.10011"_dd, "1.10001"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::SHORT,
                                                  "0"_dd, "1"_dd);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    PriceData nextTick("1.10001"_dd, "1.09991"_dd, std::chrono::system_clock::now(), "EURUSD");
    auto exit = trading::exit_rules::checkExit(trade->second, nextTick);
    XCTAssertTrue(exit.has_value(), "SHORT TP should fire when ask hits entry-ask - 1 pip");
    XCTAssertEqual(*exit, nextTick.ask, "SHORT TP closes at the current ask");
    XCTAssertEqual(*exit, "1.10001"_dd, "SHORT TP close price should equal next-tick ask");
}

// Cross-symbol tick must not close a EURUSD trade. Without the symbol filter
// in reviewStopAndLimit, an AUSIDXAUD price (~7000) fed through checkExit
// against a EURUSD LONG (exit reference ~1.10, scale 10000) trivially trips
// the stop because the bid is thousands above the limit price and thousands
// below… well, it doesn't matter which side fires — the point is that any
// non-matching tick lands far outside the EURUSD price band, so without the
// guard the trade would close at a nonsensical price.
- (void)testReviewStopAndLimit_SkipsTradesForOtherSymbols {
    PriceData entryTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);

    PriceData ausTick("7000.0"_dd, "7000.0"_dd, std::chrono::system_clock::now(), "AUSIDXAUD");
    trading::reviewStopAndLimit(*self.manager, ausTick);

    XCTAssertEqual(self.manager->reviewAccount(), 1,
                   "EURUSD trade must remain open when an AUSIDXAUD tick arrives");
    auto trades = self.manager->getActiveTrades();
    XCTAssertNotEqual(trades.find(tradeId), trades.end(),
                      "EURUSD trade should still be in active trades");
}

// Matching-symbol tick still closes — the filter must not over-block exits
// when the tick's symbol matches the trade's symbol.
- (void)testReviewStopAndLimit_MatchingSymbolTickClosesTrade {
    PriceData entryTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);

    PriceData stopTick("1.1000"_dd, "1.0999"_dd, std::chrono::system_clock::now(), "EURUSD");
    trading::reviewStopAndLimit(*self.manager, stopTick);

    auto active = self.manager->getActiveTrades();
    XCTAssertEqual(active.find(tradeId), active.end(),
                   "EURUSD trade should have been removed from active trades");

    const auto& closed = self.manager->getClosedTrades();
    XCTAssertEqual(closed.size(), 1, "Exactly one trade should be in closed trades");
    XCTAssertEqual(closed.front().id, tradeId, "Closed trade id should match the opened trade");
    XCTAssertEqual(closed.front().closePrice, stopTick.bid,
                   "LONG close price should equal the EURUSD tick bid");
    XCTAssertEqual(closed.front().closePrice, "1.0999"_dd,
                   "Recorded close price should be 1.0999");
}

// Symmetric cross-symbol case: an AUSIDXAUD trade (scale 1, price ~7000) must
// not close on a EURUSD tick (~1.10). Without the guard, the EURUSD bid sits
// far below the AUSIDXAUD stop level and would trip `tick.bid <= stopPrice`
// for a LONG, closing the trade at a nonsensical EURUSD price.
- (void)testReviewStopAndLimit_AusTradeNotClosedByEurUsdTick {
    PriceData entryTick("7000.5"_dd, "7000.0"_dd, std::chrono::system_clock::now(), "AUSIDXAUD");
    std::string tradeId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);

    PriceData eurTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    trading::reviewStopAndLimit(*self.manager, eurTick);

    XCTAssertEqual(self.manager->reviewAccount(), 1,
                   "AUSIDXAUD trade must remain open when a EURUSD tick arrives");
    auto trades = self.manager->getActiveTrades();
    XCTAssertNotEqual(trades.find(tradeId), trades.end(),
                      "AUSIDXAUD trade should still be in active trades");
}

- (void)testHasActiveTradeForSymbol_EmptyManagerReturnsFalse {
    XCTAssertFalse(self.manager->hasActiveTradeForSymbol("EURUSD"),
                   "Empty TradeManager must report no active trade for any symbol");
}

- (void)testHasActiveTradeForSymbol_TrueForOpenedSymbolFalseForOther {
    PriceData tick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);

    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "EURUSD should be reported as active after openTrade");
    XCTAssertFalse(self.manager->hasActiveTradeForSymbol("GBPUSD"),
                   "GBPUSD must not be reported as active when only EURUSD is open");
}

- (void)testHasActiveTradeForSymbol_FalseAfterCloseTrade {
    PriceData tick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "Pre-condition: EURUSD should be active before close");

    bool closed = self.manager->closeTrade(tradeId, "1.1000"_dd, tick);
    XCTAssertTrue(closed, "closeTrade should succeed for an open trade id");
    XCTAssertFalse(self.manager->hasActiveTradeForSymbol("EURUSD"),
                   "EURUSD must no longer be active after the trade is closed");
}

- (void)testCanOpenTradesOnDifferentSymbolsSimultaneously {
    PriceData eurTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    self.manager->openTrade(eurTick, "1.0"_dd, Direction::LONG);
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "EURUSD should be active after the first open");
    XCTAssertFalse(self.manager->hasActiveTradeForSymbol("AUSIDXAUD"),
                   "AUSIDXAUD must not be active before its trade is opened");

    PriceData ausTick("7000.1"_dd, "7000.0"_dd, std::chrono::system_clock::now(), "AUSIDXAUD");
    self.manager->openTrade(ausTick, "1.0"_dd, Direction::LONG);

    XCTAssertEqual(self.manager->getActiveTrades().size(), 2,
                   "Both EURUSD and AUSIDXAUD trades should be active simultaneously");
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "EURUSD should still be active after opening AUSIDXAUD");
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("AUSIDXAUD"),
                  "AUSIDXAUD should be active after its trade is opened");
}

// TradeManager::openTrade (source/trading/tradeManager.cpp:23-38) does not
// enforce same-symbol uniqueness, so callers (e.g. source/operations.cpp:57)
// rely on hasActiveTradeForSymbol to gate same-symbol re-entry. This test
// pins the invariant that a single open is enough to flip the helper to true.
- (void)testHelperStillReportsSymbolActiveAfterFirstOpen {
    PriceData tick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "EURUSD must report as active after a single openTrade — production "
                  "re-entry gating depends on this");
}

#pragma mark - Entry-side bid/ask handling

// Simple round-number spread (ask 100, bid 99) to pin which side of the spread
// each direction uses on entry. A LONG buys at the ask, but its stop/limit must
// be measured from the bid (the price it would exit at), so:
//   entryPrice == ask, exitReferencePrice == bid.
// entryBid/entryAsk record the raw spread regardless of direction.
- (void)testOpenTrade_LongEntersAtAskAndRecordsSpread {
    PriceData tick("100.0"_dd, "99.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, "1.0"_dd, Direction::LONG);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    XCTAssertEqual(trade->second.entryPrice, "100.0"_dd, "LONG executes at the ask");
    XCTAssertEqual(trade->second.exitReferencePrice, "99.0"_dd,
                   "LONG exit reference is the entry bid (close side)");
    XCTAssertEqual(trade->second.entryAsk, "100.0"_dd, "entryAsk records the tick ask");
    XCTAssertEqual(trade->second.entryBid, "99.0"_dd, "entryBid records the tick bid");
}

// Symmetric SHORT: sells at the bid, exit reference is the ask.
- (void)testOpenTrade_ShortEntersAtBidAndRecordsSpread {
    PriceData tick("100.0"_dd, "99.0"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, "1.0"_dd, Direction::SHORT);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");

    XCTAssertEqual(trade->second.entryPrice, "99.0"_dd, "SHORT executes at the bid");
    XCTAssertEqual(trade->second.exitReferencePrice, "100.0"_dd,
                   "SHORT exit reference is the entry ask (close side)");
    XCTAssertEqual(trade->second.entryAsk, "100.0"_dd, "entryAsk records the tick ask");
    XCTAssertEqual(trade->second.entryBid, "99.0"_dd, "entryBid records the tick bid");
}

#pragma mark - Operations::run loop invariants

// Operations::run gates same-symbol re-entry on hasActiveTradeForSymbol and
// runs reviewStopAndLimit *before* the entry check on each tick, so a trade
// that stops out on a tick frees its symbol for re-entry on a later tick.
// This drives that exact sequence through the public building blocks the loop
// uses (reviewStopAndLimit + hasActiveTradeForSymbol) without invoking the
// random strategy, pinning the ordering the smoke tests below cannot observe.
- (void)testRunLoopInvariant_ExitFreesSymbolForReentry {
    PriceData entryTick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    std::string firstId = self.manager->openTrade(entryTick, "1.0"_dd, Direction::LONG,
                                                  "1"_dd, "1"_dd);
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "EURUSD must be active after the first open — re-entry is gated on this");

    // Stop tick: bid drops 1 pip below the entry bid, so the LONG stops out.
    PriceData stopTick("1.1000"_dd, "1.0999"_dd, std::chrono::system_clock::now(), "EURUSD");
    trading::reviewStopAndLimit(*self.manager, stopTick);

    XCTAssertFalse(self.manager->hasActiveTradeForSymbol("EURUSD"),
                   "Symbol must be free again once the trade stops out");
    const auto& closed = self.manager->getClosedTrades();
    XCTAssertEqual(closed.size(), 1, "Exactly one trade should have closed");
    XCTAssertEqual(closed.front().closePrice, stopTick.bid,
                   "LONG closes at the tick bid (the exit side)");

    // The gate is open, so the loop would now allow a fresh entry on this symbol.
    std::string secondId = self.manager->openTrade(stopTick, "1.0"_dd, Direction::LONG,
                                                   "1"_dd, "1"_dd);
    XCTAssertNotEqual(firstId, secondId, "Re-entry must be a distinct trade");
    XCTAssertTrue(self.manager->hasActiveTradeForSymbol("EURUSD"),
                  "EURUSD active again after re-entry");
}

#pragma mark - Operations::run smoke tests

// Operations::run owns its TradeManager internally and reports only to stdout
// and Elasticsearch, so there is no return value to assert against. These are
// deliberately smoke tests: with ELASTIC_ENABLED=0 (see setUp) the run must
// drive the full per-tick loop, summarise, and results path without throwing.
// The deterministic bid/ask behaviour the loop relies on is covered by the
// building-block tests above; RandomStrategy makes per-trade outcomes here
// non-deterministic, so only the no-throw contract is checked.

- (void)testOperationsRun_EmptyTicks_DoesNotThrow {
    const std::vector<PriceData> ticks;
    const auto config = makeRandomStrategyConfig();
    XCTAssertNoThrow(Operations::run(ticks, config),
                     "run must handle an empty tick stream without throwing");
}

- (void)testOperationsRun_SingleTick_DoesNotThrow {
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD"),
    };
    const auto config = makeRandomStrategyConfig();
    XCTAssertNoThrow(Operations::run(ticks, config),
                     "run must process a single tick without throwing");
}

- (void)testOperationsRun_MultipleTicks_DoesNotThrow {
    // A drifting EURUSD series so the 10-pip SL/TP can actually fire across the
    // run, exercising both the entry and the reviewStopAndLimit exit paths.
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("1.1011"_dd, "1.1010"_dd, now, "EURUSD"),
        PriceData("1.1021"_dd, "1.1020"_dd, now, "EURUSD"),
        PriceData("1.1006"_dd, "1.1005"_dd, now, "EURUSD"),
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),
    };
    const auto config = makeRandomStrategyConfig();
    XCTAssertNoThrow(Operations::run(ticks, config),
                     "run must process a multi-tick stream without throwing");
}

#pragma mark - Operations run loop (deterministic, end-to-end)

// These drive trading::runTicks — the exact per-tick loop Operations::run
// executes — with a deterministic injected strategy and a TradeManager we own,
// so trade outcomes (entry side, exit side, realised PnL) can be asserted
// directly. EURUSD scale is 10000, so 1 pip == 0.0001.

// LONG entry executes at the ask; the stop/limit reference is the bid (the
// close side), and both raw spread prices are recorded on the trade.
- (void)testRunTicks_LongOpensAtAsk {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars("10"_dd, "10"_dd, "1.0"_dd);
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 1, "Exactly one trade should be open");
    XCTAssertEqual(tm.getClosedTrades().size(), 0, "Nothing should have closed");
    const Trade& trade = tm.getActiveTrades().begin()->second;
    XCTAssertTrue(trade.direction == Direction::LONG, "Trade should be LONG");
    XCTAssertEqual(trade.entryPrice, "1.1001"_dd, "LONG executes at the ask");
    XCTAssertEqual(trade.exitReferencePrice, "1.1000"_dd, "LONG exit reference is the bid");
    XCTAssertEqual(trade.entryAsk, "1.1001"_dd, "entryAsk records the tick ask");
    XCTAssertEqual(trade.entryBid, "1.1000"_dd, "entryBid records the tick bid");
}

// Symmetric SHORT: executes at the bid, exit reference is the ask.
- (void)testRunTicks_ShortOpensAtBid {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::SHORT});
    const auto vars = makeVars("10"_dd, "10"_dd, "1.0"_dd);
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 1, "Exactly one trade should be open");
    const Trade& trade = tm.getActiveTrades().begin()->second;
    XCTAssertTrue(trade.direction == Direction::SHORT, "Trade should be SHORT");
    XCTAssertEqual(trade.entryPrice, "1.1000"_dd, "SHORT executes at the bid");
    XCTAssertEqual(trade.exitReferencePrice, "1.1001"_dd, "SHORT exit reference is the ask");
    XCTAssertEqual(trade.entryAsk, "1.1001"_dd, "entryAsk records the tick ask");
    XCTAssertEqual(trade.entryBid, "1.1000"_dd, "entryBid records the tick bid");
}

// No signal -> no position, across many ticks.
- (void)testRunTicks_NoSignalOpensNothing {
    TradeManager tm;
    ScriptedStrategy strategy({});  // empty script: decide() always returns nullopt
    const auto vars = makeVars("10"_dd, "10"_dd, "1.0"_dd);
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("1.1011"_dd, "1.1010"_dd, now, "EURUSD"),
        PriceData("1.1021"_dd, "1.1020"_dd, now, "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 0, "No trade should open without a signal");
    XCTAssertEqual(tm.getClosedTrades().size(), 0, "Nothing should close either");
}

// Re-entry is gated while a position is open: an always-signalling strategy on
// flat ticks (price never reaches the 10-pip SL/TP) must still open only one
// trade for the symbol, not one per tick.
- (void)testRunTicks_ReentryGatedWhileActive {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars("10"_dd, "10"_dd, "1.0"_dd);
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 1,
                   "Gate must suppress duplicate same-symbol entries while one is open");
    XCTAssertEqual(tm.getClosedTrades().size(), 0, "Flat price must not trigger SL/TP");
}

// LONG take-profit: a 10-pip favourable move nets only 9 pips because entry was
// at the ask (1.1001) while the TP is measured from the entry bid (1.1000) and
// closes at the bid. This pins that the spread is accounted for end-to-end.
- (void)testRunTicks_LongTP_ClosesAtBid_PnlNetOfSpread {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});       // opens once, no re-entry
    const auto vars = makeVars("0"_dd, "10"_dd, "1.0"_dd);   // TP only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),   // open LONG @ ask 1.1001
        PriceData("1.1011"_dd, "1.1010"_dd, now, "EURUSD"),   // bid 1.1010 hits TP (ref 1.1000 + 10p)
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 0, "Position should be closed by TP");
    XCTAssertEqual(tm.getClosedTrades().size(), 1, "Exactly one closed trade");
    const Trade& closed = tm.getClosedTrades().front();
    XCTAssertEqual(closed.closePrice, "1.1010"_dd, "LONG TP closes at the tick bid");
    XCTAssertEqual(closed.pnl, "9"_dd, "10-pip move nets 9 pips after the 1-pip spread");
}

// Symmetric SHORT take-profit: enters at the bid (1.1000), TP measured from the
// entry ask (1.1001), closes at the ask. Again 9 pips net of the spread.
- (void)testRunTicks_ShortTP_ClosesAtAsk_PnlNetOfSpread {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::SHORT});
    const auto vars = makeVars("0"_dd, "10"_dd, "1.0"_dd);   // TP only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),   // open SHORT @ bid 1.1000
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // ask 1.0991 hits TP (ref 1.1001 - 10p)
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 0, "Position should be closed by TP");
    XCTAssertEqual(tm.getClosedTrades().size(), 1, "Exactly one closed trade");
    const Trade& closed = tm.getClosedTrades().front();
    XCTAssertEqual(closed.closePrice, "1.0991"_dd, "SHORT TP closes at the tick ask");
    XCTAssertEqual(closed.pnl, "9"_dd, "10-pip move nets 9 pips after the 1-pip spread");
}

// LONG stop-loss: a 10-pip adverse move (entry bid 1.1000 down to 1.0990) loses
// 11 pips because entry was at the ask 1.1001. Loss includes the spread.
- (void)testRunTicks_LongSL_ClosesAtBid_NegativePnl {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars("10"_dd, "0"_dd, "1.0"_dd);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),   // open LONG @ ask 1.1001
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // bid 1.0990 hits SL (ref 1.1000 - 10p)
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 0, "Position should be stopped out");
    XCTAssertEqual(tm.getClosedTrades().size(), 1, "Exactly one closed trade");
    const Trade& closed = tm.getClosedTrades().front();
    XCTAssertEqual(closed.closePrice, "1.0990"_dd, "LONG SL closes at the tick bid");
    XCTAssertEqual(closed.pnl, -"11"_dd, "10-pip adverse move loses 11 pips including the spread");
}

// Exit-before-entry ordering within a single tick: on the tick that stops the
// first LONG out, reviewStopAndLimit closes it first, the symbol frees, and the
// always-LONG strategy immediately re-enters on that same tick — at the new
// tick's ask. Pins that the loop reviews exits before considering entries.
- (void)testRunTicks_ExitThenSameTickReentry {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars("10"_dd, "0"_dd, "1.0"_dd);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),   // LONG#1 @ ask 1.1001
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // stops LONG#1, re-opens LONG#2
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getClosedTrades().size(), 1, "The first LONG should have closed");
    XCTAssertEqual(tm.getActiveTrades().size(), 1, "A re-entry should be open on the same tick");

    const Trade& closed = tm.getClosedTrades().front();
    XCTAssertEqual(closed.entryPrice, "1.1001"_dd, "Closed trade was LONG#1, entered at tick1 ask");
    XCTAssertEqual(closed.closePrice, "1.0990"_dd, "LONG#1 stopped out at tick2 bid");

    const Trade& reentry = tm.getActiveTrades().begin()->second;
    XCTAssertTrue(reentry.direction == Direction::LONG, "Re-entry should be LONG");
    XCTAssertEqual(reentry.entryPrice, "1.0991"_dd, "Re-entry executes at tick2 ask");
    XCTAssertEqual(reentry.exitReferencePrice, "1.0990"_dd, "Re-entry exit reference is tick2 bid");
}

// Two symbols open simultaneously, each entering on the correct side of its own
// spread regardless of price scale (EURUSD ~1.10 vs AUSIDXAUD ~7000).
- (void)testRunTicks_MultiSymbol {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars("0"_dd, "0"_dd, "1.0"_dd);    // no exits — both persist
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("7000.5"_dd, "7000.0"_dd, now, "AUSIDXAUD"),
    };

    trading::runTicks(tm, strategy, ticks, vars);

    XCTAssertEqual(tm.getActiveTrades().size(), 2, "Both symbols should be open");
    const Trade* eur = nullptr;
    const Trade* aus = nullptr;
    for (const auto& [id, trade] : tm.getActiveTrades()) {
        if (trade.symbol == "EURUSD") eur = &trade;
        else if (trade.symbol == "AUSIDXAUD") aus = &trade;
    }
    XCTAssertTrue(eur != nullptr, "EURUSD trade should exist");
    XCTAssertTrue(aus != nullptr, "AUSIDXAUD trade should exist");
    XCTAssertEqual(eur->entryPrice, "1.1001"_dd, "EURUSD LONG enters at its ask");
    XCTAssertEqual(aus->entryPrice, "7000.5"_dd, "AUSIDXAUD LONG enters at its ask");
}

#pragma mark - Account loss limit (fail fast)

// FLOATING drawdown alone must trigger the cutoff: no stop-loss, so the open
// LONG's mark-to-market loss is the only thing the limit can see. The crash
// tick marks the trade at -101 (floor: 10000 * 1% = 100), the run stops, and
// the trade is liquidated at that mark — so the reported PnL is the true
// account PnL, not 0 realized.
- (void)testRunTicks_FloatingDrawdownBreach_LiquidatesAtMark {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG});
    const auto vars = makeVars("0"_dd, "0"_dd, "1.0"_dd);    // no SL/TP at all
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),   // open LONG @ ask 1.1001
        PriceData("1.0901"_dd, "1.0900"_dd, now, "EURUSD"),   // mark at bid: floating -101
        PriceData("1.0901"_dd, "1.0900"_dd, now, "EURUSD"),   // must never be processed
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "1"_dd};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    XCTAssertTrue(status == trading::RunStatus::LossLimitBreached,
                  "Floating drawdown must count toward the loss limit");
    XCTAssertEqual(tm.getActiveTrades().size(), 0, "The open trade must be liquidated");
    XCTAssertEqual(tm.getClosedTrades().size(), 1, "Exactly one (liquidated) closed trade");
    const Trade& closed = tm.getClosedTrades().front();
    XCTAssertEqual(closed.closePrice, "1.0900"_dd, "Liquidation closes at the marked bid");
    XCTAssertEqual(closed.pnl, -"101"_dd, "Liquidated PnL is the marked floating loss");
    XCTAssertTrue(closed.liquidated, "Forced close must carry the liquidated flag");
    XCTAssertEqual(closed.floatingPnl, "0"_dd, "floatingPnl is zeroed once realized");
    XCTAssertEqual(tm.calculatePnl(), -"101"_dd, "True account PnL is fully realized");
    XCTAssertEqual(tm.unrealizedPnl(), "0"_dd, "Nothing floating remains after liquidation");
}

// On breach, EVERY open trade is liquidated — each at its own symbol's last
// marked price, not the breaching tick's. The EURUSD crash (-101) breaches the
// -100 floor; the AUSIDXAUD position, marked only at its entry tick, closes at
// its own mark 7000.0 for the -0.5 spread cost. True PnL = -101.5.
- (void)testRunTicks_Liquidation_ClosesEverySymbolAtItsOwnMark {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars("0"_dd, "0"_dd, "1.0"_dd);    // no SL/TP at all
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),     // open EURUSD LONG
        PriceData("7000.5"_dd, "7000.0"_dd, now, "AUSIDXAUD"),  // open AUSIDXAUD LONG
        PriceData("1.0901"_dd, "1.0900"_dd, now, "EURUSD"),     // EURUSD -101: breach
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "1"_dd};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    XCTAssertTrue(status == trading::RunStatus::LossLimitBreached,
                  "Combined floating drawdown must breach the limit");
    XCTAssertEqual(tm.getActiveTrades().size(), 0, "Both positions must be liquidated");
    XCTAssertEqual(tm.getClosedTrades().size(), 2, "Both symbols produce a closed trade");
    XCTAssertEqual(tm.calculatePnl(), -"101.5"_dd,
                   "True PnL combines both liquidations (-101 EURUSD, -0.5 AUSIDXAUD spread)");
    for (const Trade& closed : tm.getClosedTrades()) {
        XCTAssertTrue(closed.liquidated, "Every forced close must carry the liquidated flag");
        if (closed.symbol == "EURUSD") {
            XCTAssertEqual(closed.closePrice, "1.0900"_dd, "EURUSD closes at the crash bid");
            XCTAssertEqual(closed.pnl, -"101"_dd, "EURUSD realizes the crash drawdown");
        } else {
            XCTAssertEqual(closed.closePrice, "7000.0"_dd,
                           "AUSIDXAUD closes at its own last mark, not a EURUSD price");
            XCTAssertEqual(closed.pnl, -"0.5"_dd, "AUSIDXAUD realizes only its spread cost");
        }
    }
}

// MAX_OPEN_TRADES caps simultaneous positions across the whole run: with a
// cap of 1, the second symbol's signal is skipped while the first position is
// open. The uncapped variant of this setup is testRunTicks_MultiSymbol.
- (void)testRunTicks_MaxOpenTradesCap_BlocksSecondEntry {
    TradeManager tm;
    ScriptedStrategy strategy({Direction::LONG, Direction::LONG});
    const auto vars = makeVars("0"_dd, "0"_dd, "1.0"_dd);    // no exits — first stays open
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("7000.5"_dd, "7000.0"_dd, now, "AUSIDXAUD"),
    };
    const trading::RiskLimits limits{.maxOpenTrades = 1};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    XCTAssertTrue(status == trading::RunStatus::Completed, "Cap is not a failure condition");
    XCTAssertEqual(tm.getActiveTrades().size(), 1, "Cap of 1 must block the second entry");
    XCTAssertEqual(tm.getActiveTrades().begin()->second.symbol, std::string("EURUSD"),
                   "The first signal wins the only slot");
}

// A stop-out that breaches the loss limit must end the run on that tick,
// BEFORE the entry phase — so unlike testRunTicks_ExitThenSameTickReentry the
// always-LONG strategy gets no same-tick re-entry, and later ticks never run.
// Floor here: 10000 * 0.1% = 10; the single stop-out loses 11 (incl. spread).
- (void)testRunTicks_LossLimitBreach_StopsRunBeforeReentry {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars("10"_dd, "0"_dd, "1.0"_dd);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),   // LONG#1 @ ask 1.1001
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // stops LONG#1: pnl -11, breach
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // must never be processed
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "0.1"_dd};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    XCTAssertTrue(status == trading::RunStatus::LossLimitBreached,
                  "Run must report the loss-limit breach");
    XCTAssertEqual(tm.getClosedTrades().size(), 1, "Only the stopped-out trade should exist");
    XCTAssertFalse(tm.getClosedTrades().front().liquidated,
                   "A stop-out is an organic close, not a liquidation");
    XCTAssertEqual(tm.getActiveTrades().size(), 0,
                   "Breach is checked before entries — no re-entry may open");
    XCTAssertEqual(tm.calculatePnl(), -"11"_dd, "Realized PnL at cutoff is the single stop-out");
}

// A realized loss inside the limit must not stop the run: same stop-out, but a
// 5% limit (floor -500) comfortably absorbs -11, so the run completes and the
// same-tick re-entry happens as normal.
- (void)testRunTicks_LossWithinLimit_RunsToCompletion {
    TradeManager tm;
    AlwaysLongStrategy strategy;
    const auto vars = makeVars("10"_dd, "0"_dd, "1.0"_dd);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // stop-out -11, within -500
    };
    const trading::RiskLimits limits{.startingBalance = "10000"_dd,
                                     .maxLossPercent = "5"_dd};

    const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

    XCTAssertTrue(status == trading::RunStatus::Completed, "Run should not be cut off");
    XCTAssertEqual(tm.getClosedTrades().size(), 1, "The stop-out still closes");
    XCTAssertEqual(tm.getActiveTrades().size(), 1, "Same-tick re-entry proceeds as normal");
}

// maxLossPercent <= 0 disables the check entirely — losses far past any
// percentage are ignored and the run completes (the experimentation escape
// hatch, no extra flag needed). Cover both 0 and -1 spellings.
- (void)testRunTicks_LossLimitDisabled_ZeroAndNegative {
    const auto vars = makeVars("10"_dd, "0"_dd, "1.0"_dd);   // SL only
    const auto now = std::chrono::system_clock::now();
    const std::vector<PriceData> ticks{
        PriceData("1.1001"_dd, "1.1000"_dd, now, "EURUSD"),
        PriceData("1.0991"_dd, "1.0990"_dd, now, "EURUSD"),   // stop-out -11
    };

    for (const auto percent : {"0"_dd, -"1"_dd}) {
        TradeManager tm;
        AlwaysLongStrategy strategy;
        // Tiny balance: -11 realized is over 100% of the account, yet with the
        // limit disabled the run must still complete.
        const trading::RiskLimits limits{.startingBalance = "10"_dd,
                                         .maxLossPercent = percent};

        const auto status = trading::runTicks(tm, strategy, ticks, vars, limits);

        XCTAssertTrue(status == trading::RunStatus::Completed,
                      "maxLossPercent <= 0 must disable the cutoff");
        XCTAssertEqual(tm.getActiveTrades().size(), 1, "Re-entry proceeds unchecked");
    }
}

@end
