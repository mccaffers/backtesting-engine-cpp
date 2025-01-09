// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <iostream>
#include <vector>
#include <string>

class Base64 {
public:
  const std::string b64encode(const void* data, const size_t &len);
  const std::string b64decode(const void* data, const size_t &len);
  std::string b64encode(const std::string& str);
  std::string b64decode(const std::string& str64);
};
