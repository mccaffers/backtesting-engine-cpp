// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/tradingDefinitions/config/configuration.hpp"

export module backtestRunner;

import std;                 // replaces <cstdio>, <span>, <sstream>, <string>, <vector>
import priceData;           // PriceData
import operations;          // Operations
import sqlManager;          // SqlManager
import connectionFactory;   // questdb::connectionFromEnv
import databaseConnection;  // DatabaseConnection
import tickCache;           // tick_cache::Superset

namespace {

// Split a comma-separated symbols string into the list SqlManager expects.
std::vector<std::string> splitSymbols(const std::string& symbolsCsv) {
  std::vector<std::string> symbols;
  std::istringstream ss(symbolsCsv);
  for (std::string token; std::getline(ss, token, ',');) {
    symbols.push_back(token);
  }
  return symbols;
}

}  // namespace

// Pulls all tick data for the run's symbols/window out of QuestDB. The window
// is lastMonths long and ends offsetMonths before now (0 = the present day).
// Expensive — call once per run and reuse the result across that run's
// strategies.
export std::vector<PriceData> loadTicks(const std::string& questdbHost,
                                        const std::string& symbolsCsv,
                                        int lastMonths,
                                        int offsetMonths) {

  // Host comes from argv; the port (default 8812, overridable via
  // QUESTDB_PORT) and credentials come from the shared factory, so a
  // non-default instance (e.g. a converted dataset) can be targeted without a
  // rebuild.
  const DatabaseConnection db = questdb::connectionFromEnv(questdbHost);

  // Get all the tick data out of QuestDB for these symbols
  std::vector<PriceData> ticks =
      SqlManager::loadPriceData(db, splitSymbols(symbolsCsv), lastMonths, offsetMonths);

  std::printf("Total ticks streamed: %zu\n", ticks.size());

  return ticks;
}

// Pulls `months` of history ending at one QuestDB-computed snapshot instant T,
// with the month boundaries that produced it: the boundary row is fetched
// first (QuestDB's own dateadd semantics, one now() per statement), then the
// ticks are loaded with literal bounds from that row, so the data and the
// boundaries used to slice it can never disagree. One superset serves every
// rolling-ladder window as a contiguous slice — see tick_cache.
export tick_cache::Superset loadTickSuperset(const std::string& questdbHost,
                                             const std::string& symbolsCsv,
                                             int months) {
  const DatabaseConnection db = questdb::connectionFromEnv(questdbHost);
  const std::vector<std::string> symbols = splitSymbols(symbolsCsv);

  auto boundaries = SqlManager::loadMonthBoundaries(db, months);
  std::vector<PriceData> ticks = SqlManager::loadPriceDataBetween(
      db, symbols, boundaries[static_cast<std::size_t>(months)], boundaries[0]);

  std::printf("Superset loaded: %zu ticks, %d months ending %s\n",
              ticks.size(), months,
              SqlManager::formatTimestamp(boundaries[0]).c_str());

  return tick_cache::Superset{std::move(ticks), std::move(boundaries)};
}

// Runs one backtest against already-loaded ticks (no QuestDB access).
// chainWindows opts the run into the rolling-window ladder (queue path only —
// see Operations::run).
export void runBacktestOnTicks(std::span<const PriceData> ticks,
                               const tradingDefinitions::Configuration& config,
                               bool chainWindows = false) {
  Operations::run(ticks, config, chainWindows);
}

// Convenience for the direct path: loads ticks then runs a single backtest.
export int runBacktest(const std::string& questdbHost,
                       const tradingDefinitions::Configuration& config) {
  const std::vector<PriceData> ticks =
      loadTicks(questdbHost, config.SYMBOLS, config.LAST_MONTHS, config.OFFSET_MONTHS);
  runBacktestOnTicks(ticks, config);
  return 0;
}
