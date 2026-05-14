// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import <boost/decimal/literals.hpp>
#import "tradeManager.hpp"
#import "exitRules.hpp"

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
    bool closed = self.manager->closeTrade(tradeId, "110.0"_dd);
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

@end
