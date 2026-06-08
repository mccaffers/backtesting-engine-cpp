// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace trading_definitions {
// Run-level descriptor: the unit of QuestDB tick data shared by every strategy
// in a sweep. Carried on BACKTESTING_QUEUE_RUN and linked to its strategies via
// RUN_ID (see queueKeys.hpp). The runner reassembles a full Configuration from
// this plus each popped Strategy.
struct RunConfiguration {
  std::string RUN_ID;
  std::string SYMBOLS;
  int LAST_MONTHS;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RunConfiguration,
                                   RUN_ID,
                                   SYMBOLS,
                                   LAST_MONTHS
                                   );

};
