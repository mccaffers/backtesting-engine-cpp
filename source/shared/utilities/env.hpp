// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

namespace env {

// Returns $name, or `fallback` when the variable is unset or empty.
std::string getOr(const char* name, std::string fallback);

}  // namespace env
