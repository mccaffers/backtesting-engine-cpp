// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import <boost/decimal/literals.hpp>
#import "tradeManager.hpp"
#import "exitRules.hpp"
#import "reviewStopAndLimit.hpp"

// Pulls in the _dd user-defined literal so "1.23"_dd produces a decimal64_t
// directly. decimal64_t has no implicit conversion from double — the closest
// C# analogue is having to write `1.23m` instead of `1.23` for a decimal.
using namespace boost::decimal::literals;

@interface TradeManagerTests : XCTestCase
@property (nonatomic) TradeManager* manager;
@end

@implementation TradeManagerTests

- (void)setUp {
    self.manager = new TradeManager();
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

@end
