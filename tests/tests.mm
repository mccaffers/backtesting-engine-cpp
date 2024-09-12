// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import "application.hpp"

@interface tests : XCTestCase
@end

@implementation tests

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

- (void)testExample {
  // This is an example of a functional test case.
  // Use XCTAssert and related functions to verify your tests produce the correct results.
  std::vector<int> numbers = {-1, -2, 3, 4, -5};
  XCTAssert(Application().addNumbers(numbers) == -1);
}

@end
