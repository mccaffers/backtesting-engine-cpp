// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <iostream>
#include <vector>
#include <memory>
#include "ConfigManager.hpp"
#include "Application.hpp"
#include "ServiceA.hpp"

int main(int argc, const char * argv[]) {
  // insert code here...
  std::cout << "Hello, World!\n";
  std::vector<int> numbers = {-1, -2, 3, 4, -5};
  std::cout << Application().addNumbers(numbers) << std::endl;
  
  auto configManager = ConfigManager::getInstance();
  
  ServiceA serviceA(configManager);

  serviceA.doSomething();

  configManager->setConfig("New Configuration");

  serviceA.doSomething();
  
  return 0;
}



