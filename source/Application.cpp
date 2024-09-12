// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "Application.hpp"

int Application::addNumbers(const std::vector<int>& numbers) {
  int sum = 0;
  for (int num : numbers) {
      sum += num;
  }
  return sum;
}
