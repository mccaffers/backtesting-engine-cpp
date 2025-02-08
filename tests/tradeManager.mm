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
    TradeManager::reset();  // Reset the singleton instance
    self.manager = TradeManager::getInstance();
}

- (void)tearDown {
    self.manager->clearAllTrades();
    TradeManager::reset();
}

- (void)testOpenTrade {
    std::string tradeId = self.manager->openTrade(100.0, 1.0, true);
    XCTAssertFalse(tradeId.empty(), "Trade ID should not be empty");
    XCTAssertEqual(self.manager->reviewAccount(), 1, "Should have 1 active trade");
}

- (void)testCloseTrade {
    std::string tradeId = self.manager->openTrade(100.0, 1.0, true);
    bool closed = self.manager->closeTrade(tradeId);
    XCTAssertTrue(closed, "Trade should be closed successfully");
    XCTAssertEqual(self.manager->reviewAccount(), 0, "Should have 0 active trades");
}

- (void)testMultipleTrades {
    self.manager->openTrade(100.0, 1.0, true);
    self.manager->openTrade(200.0, 2.0, false);
    self.manager->openTrade(300.0, 3.0, true);
    
    XCTAssertEqual(self.manager->reviewAccount(), 3, "Should have 3 active trades");
}

- (void)testTradeDetails {
    std::string tradeId = self.manager->openTrade(100.0, 1.0, true);
    auto trades = self.manager->getActiveTrades();
    auto trade = trades.find(tradeId);
    
    XCTAssertNotEqual(trade, trades.end(), "Trade should exist");
    XCTAssertEqual(trade->second.entryPrice, 100.0, "Entry price should match");
    XCTAssertEqual(trade->second.size, 1.0, "Size should match");
    XCTAssertTrue(trade->second.isLong, "Trade should be long");
}

@end
