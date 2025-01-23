// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------


#import <XCTest/XCTest.h>
#import "application.hpp"
#import "databaseConnection.hpp"

// Using the Objective-C Framework to build XCTestCases
// This code is a mix of Objective-C and C++, commonly referred to as Objective-C++.
// This is possible because .mm files in Apple's ecosystem allow Objective-C and C++ code to coexist.

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
