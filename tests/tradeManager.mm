// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import "tradeManager.hpp"

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
    PriceData tick(100.0, 99.0, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, 1.0, Direction::LONG);
    XCTAssertFalse(tradeId.empty(), "Trade ID should not be empty");
    XCTAssertEqual(self.manager->reviewAccount(), 1, "Should have 1 active trade");
}

- (void)testCloseTrade {
    PriceData tick(100.0, 99.0, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, 1.0, Direction::LONG);
    bool closed = self.manager->closeTrade(tradeId, 110.0);
    XCTAssertTrue(closed, "Trade should be closed successfully");
    XCTAssertEqual(self.manager->reviewAccount(), 0, "Should have 0 active trades");
}

- (void)testMultipleTrades {
    PriceData tick1(100.0, 99.0, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick2(200.0, 199.0, std::chrono::system_clock::now(), "EURUSD");
    PriceData tick3(300.0, 299.0, std::chrono::system_clock::now(), "EURUSD");
    self.manager->openTrade(tick1, 1.0, Direction::LONG);
    self.manager->openTrade(tick2, 2.0, Direction::SHORT);
    self.manager->openTrade(tick3, 3.0, Direction::LONG);
    
    XCTAssertEqual(self.manager->reviewAccount(), 3, "Should have 3 active trades");
}

- (void)testTradeDetails {
    PriceData tick(100.0, 99.0, std::chrono::system_clock::now(), "EURUSD");
    std::string tradeId = self.manager->openTrade(tick, 1.0, Direction::LONG);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");
    XCTAssertEqual(trade->second.entryPrice, 100.0, "Entry price should match");
    XCTAssertEqual(trade->second.size, 1.0, "Size should match");
    XCTAssertTrue(trade->second.direction == Direction::LONG, "Trade should be long");
}

@end
