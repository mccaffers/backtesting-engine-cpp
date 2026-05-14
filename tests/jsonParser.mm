// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import "jsonParser.hpp"
#include "base64.hpp"

@interface jsonParserTests : XCTestCase
@end

@implementation jsonParserTests

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

- (void)testValidJsonParsing {
    // Create a sample configuration JSON with all required fields
    std::string validJson = R"({
  "RUN_ID": "UNIQUE_IDENTIFER",
  "SYMBOLS": "EURUSD",
  "LAST_MONTHS": 6,
  "STRATEGY": {
      "UUID": "",
      "TRADING_VARIABLES": {
          "STRATEGY": "RandomStrategy",
          "STOP_DISTANCE_IN_PIPS": "1",
          "LIMIT_DISTANCE_IN_PIPS": "1",
          "TRADING_SIZE": "1"
      },
      "OHLC_VARIABLES": [
          {
              "OHLC_COUNT": 60,
              "OHLC_MINUTES": 100
          }
      ],
      "STRATEGY_VARIABLES" : {
        "OHLC_RSI_VARIABLES": {
            "RSI_LONG": 60,
            "RSI_SHORT": 40
        }
      }
  }
})";
    
    // Convert to base64
    std::string base64Input = Base64::b64encode(validJson);
    
    // Test parsing
    trading_definitions::Configuration result = JsonParser::parseConfigurationFromBase64(base64Input);
    XCTAssertFalse(result.RUN_ID.empty(), "Parsing should succeed with valid JSON");
}


@end
