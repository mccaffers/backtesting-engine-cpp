// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// std headers
#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <iomanip>

// external headers
#include <nlohmann/json.hpp>

// backtesting engine headers
#include "configManager.hpp"
#include "serviceA.hpp"
#include "databaseConnection.hpp"
#include "base64.hpp"
#include "trading_definitions.hpp"  // For everything
#include "tradeManager.hpp"
#include "jsonParser.hpp"
#include "sqlManager.hpp"
#include "operations.hpp"

using json = nlohmann::json;

// Entry point. Expects two command-line arguments:
//   argv[1] — hostname/IP of the QuestDB instance
//   argv[2] — Base64-encoded JSON strategy configuration
int main(int argc, const char * argv[]) {
  
  // Validate required command-line arguments before proceeding
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <questdb-host> <base64-config>" << std::endl;
    return 1;
  }

  // Connect to QuestDB on the default port (8812) using default credentials
  DatabaseConnection db(argv[1], 8812, "qdb", "admin", "quest");

  // Decode and apply the strategy configuration from Base64-encoded JSON
  JsonParser::parseConfigurationFromBase64(argv[2]);

  // Define the instruments to backtest and stream their historical tick data
  std::vector<std::string> symbols = {"AUSIDXAUD", "EURUSD"};
  std::vector<PriceData> ticks = SqlManager::streamPriceData(db, symbols, 3);
  printf("Total ticks streamed: %zu\n", ticks.size());

  // Execute the backtest by replaying all ticks through the strategy logic
  Operations::run(ticks);

  return 0;
  
}
