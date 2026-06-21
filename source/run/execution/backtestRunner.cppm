// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/utilities/env.hpp"
#include "shared/tradingDefinitions/config/configuration.hpp"

export module backtestRunner;

import std;                 // replaces <cstdio>, <sstream>, <string>, <vector>
import priceData;           // PriceData
import operations;          // Operations
import sqlManager;          // SqlManager
import databaseConnection;  // DatabaseConnection

// Pulls all tick data for the run's symbols/window out of QuestDB. Expensive —
// call once per run and reuse the result across that run's strategies.
export std::vector<PriceData> loadTicks(const std::string& questdbHost,
                                        const std::string& symbolsCsv,
                                        int lastMonths) {

  // QuestDB pg-wire port: defaults to 8812 but is overridable via QUESTDB_PORT
  // so a non-default instance (e.g. a converted dataset) can be targeted
  // without a rebuild.
  const int questdbPort = std::stoi(env::getOr("QUESTDB_PORT", "8812"));
  DatabaseConnection db(questdbHost, questdbPort, "qdb", "admin", "quest");

  // Get a list of symbols
  std::vector<std::string> symbols;
  std::istringstream ss(symbolsCsv);
  for (std::string token; std::getline(ss, token, ',');) {
    symbols.push_back(token);
  }

  // Get all the tick data out of QuestDB for these symbols
  std::vector<PriceData> ticks =
      SqlManager::loadPriceData(db, symbols, lastMonths);

  std::printf("Total ticks streamed: %zu\n", ticks.size());

  return ticks;
}

// Runs one backtest against already-loaded ticks (no QuestDB access).
export void runBacktestOnTicks(const std::vector<PriceData>& ticks,
                               const tradingDefinitions::Configuration& config) {
  Operations::run(ticks, config);
}

// Convenience for the direct path: loads ticks then runs a single backtest.
export int runBacktest(const std::string& questdbHost,
                       const tradingDefinitions::Configuration& config) {
  const std::vector<PriceData> ticks =
      loadTicks(questdbHost, config.SYMBOLS, config.LAST_MONTHS);
  runBacktestOnTicks(ticks, config);
  return 0;
}
