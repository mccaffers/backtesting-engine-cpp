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
  static const std::string b64encode(const void* data, const size_t &len);
  static const std::string b64decode(const void* data, const size_t &len);
  static std::string b64encode(const std::string& str);
  static std::string b64decode(const std::string& str64);
  static bool isValidBase64(const std::string& input);
  static std::string checkInput(const std::string& base64_input);
};
