// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "backtestRunner.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "databaseConnection.hpp"
#include "operations.hpp"
#include "sqlManager.hpp"

int runBacktest(const std::string& questdbHost,
                const trading_definitions::Configuration& config) {
  DatabaseConnection db(questdbHost, 8812, "qdb", "admin", "quest");

  std::vector<std::string> symbols;
  std::istringstream ss(config.SYMBOLS);
  for (std::string token; std::getline(ss, token, ',');) {
    symbols.push_back(token);
  }

  std::vector<PriceData> ticks =
      SqlManager::streamPriceData(db, symbols, config.LAST_MONTHS);

  printf("Total ticks streamed: %zu\n", ticks.size());

  Operations::run(ticks, config);
  return 0;
}
