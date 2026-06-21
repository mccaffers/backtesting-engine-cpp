// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <cstddef>
#include <string>

class Base64 {
public:
  static const std::string b64encode(const unsigned char* data, const size_t &len);
  static const std::string b64decode(const unsigned char* data, const size_t &len);
  static std::string b64encode(const std::string& str);
  static std::string b64decode(const std::string& str64);
};

