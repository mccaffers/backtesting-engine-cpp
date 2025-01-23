// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// Using the Objective-C Framework to build XCTestCases

#import <XCTest/XCTest.h>
#import "application.hpp"
#import "databaseConnection.hpp"

@interface dbtests : XCTestCase
@end

@implementation dbtests

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

- (void)testExample {
  
  DatabaseConnection db("test");
  std::string endpoint = "test";
  std::string connection_string =
      "host=" + endpoint + " "
      "port=8812 "
      "dbname=qdb "
      "user=admin "
      "password=quest "
      "connect_timeout=3";
  XCTAssertEqual(connection_string,  db.getConnectionString());
  
  
}

@end
