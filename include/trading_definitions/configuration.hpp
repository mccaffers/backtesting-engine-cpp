// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "strategy.hpp"

namespace trading_definitions {
struct Configuration {
  std::string RUN_ID;
  std::string SYMBOLS;
  int LAST_MONTHS;
  Strategy STRATEGY;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Configuration,
                                   RUN_ID,
                                   SYMBOLS,
                                   LAST_MONTHS,
                                   STRATEGY
                                   );

};
