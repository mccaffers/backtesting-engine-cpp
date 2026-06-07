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

std::vector<PriceData> loadTicks(const std::string& questdbHost,
                                 const std::string& symbolsCsv,
                                 int lastMonths) {

  DatabaseConnection db(questdbHost, 8812, "qdb", "admin", "quest");

  // Get a list of symbols
  std::vector<std::string> symbols;
  std::istringstream ss(symbolsCsv);
  for (std::string token; std::getline(ss, token, ',');) {
    symbols.push_back(token);
  }

  // Get all the tick data out of QuestDB for these symbols
  std::vector<PriceData> ticks =
      SqlManager::streamPriceData(db, symbols, lastMonths);

  printf("Total ticks streamed: %zu\n", ticks.size());

  return ticks;
}

void runBacktestOnTicks(const std::vector<PriceData>& ticks,
                        const trading_definitions::Configuration& config) {
  Operations::run(ticks, config);
}

int runBacktest(const std::string& questdbHost,
                const trading_definitions::Configuration& config) {
  const std::vector<PriceData> ticks =
      loadTicks(questdbHost, config.SYMBOLS, config.LAST_MONTHS);
  runBacktestOnTicks(ticks, config);
  return 0;
}
