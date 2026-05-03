// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import <string>
#import "symbolScale.hpp"

@interface SymbolScaleTests : XCTestCase
@end

@implementation SymbolScaleTests

- (void)testFourDigitFxPairs {
    XCTAssertEqual(symbol_scale::get("EURUSD"), 10000);
    XCTAssertEqual(symbol_scale::get("AUDUSD"), 10000);
    XCTAssertEqual(symbol_scale::get("GBPUSD"), 10000);
    XCTAssertEqual(symbol_scale::get("USDCAD"), 10000);
    XCTAssertEqual(symbol_scale::get("EURNOK"), 10000);
}

- (void)testJpyPairs {
    XCTAssertEqual(symbol_scale::get("USDJPY"), 100);
    XCTAssertEqual(symbol_scale::get("GBPJPY"), 100);
    XCTAssertEqual(symbol_scale::get("EURJPY"), 100);
}

- (void)testIndicesAndCommodities {
    XCTAssertEqual(symbol_scale::get("USA500IDXUSD"), 1);
    XCTAssertEqual(symbol_scale::get("USATECHIDXUSD"), 1);
    XCTAssertEqual(symbol_scale::get("XAUUSD"), 1);
    XCTAssertEqual(symbol_scale::get("BRENTCMDUSD"), 1);
}

- (void)testBoundaryEntries {
    XCTAssertEqual(symbol_scale::get("AUDNZD"), 10000);
    XCTAssertEqual(symbol_scale::get("XAGUSD"), 1);
}

- (void)testUnknownSymbolReturnsSentinel {
    XCTAssertEqual(symbol_scale::get("NOPE"), symbol_scale::kUnknown);
    XCTAssertEqual(symbol_scale::get(""), symbol_scale::kUnknown);
    XCTAssertEqual(symbol_scale::get("EURUSDX"), symbol_scale::kUnknown);
}

- (void)testCaseSensitive {
    XCTAssertEqual(symbol_scale::get("eurusd"), symbol_scale::kUnknown);
}

- (void)testAcceptsStdString {
    std::string s = "USDCHF";
    XCTAssertEqual(symbol_scale::get(s), 10000);
}

- (void)testCompileTimeFolding {
    constexpr int eurusd = symbol_scale::get("EURUSD");
    constexpr int usdjpy = symbol_scale::get("USDJPY");
    static_assert(eurusd == 10000);
    static_assert(usdjpy == 100);
    XCTAssertEqual(eurusd, 10000);
    XCTAssertEqual(usdjpy, 100);
}

@end
