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

using json = nlohmann::json;

// Entry point. Expects two command-line arguments:
//   argv[1] — hostname/IP of the QuestDB instance
//   argv[2] — Base64-encoded JSON strategy configuration
int main(int argc, const char * argv[]) {
  
  DatabaseConnection db(argv[1], 8812, "qdb", "admin", "quest");

  JsonParser::parseConfigurationFromBase64(argv[2]);

  std::vector<PriceData> ticks = SqlManager::streamPriceData(db, 1);
  printf("Total ticks streamed: %zu\n", ticks.size());

  // print first tick
  auto time_t = std::chrono::system_clock::to_time_t(ticks[0].timestamp);
  struct tm tm = {};
  if (localtime_r(&time_t, &tm) == nullptr) {
      std::cerr << "Error: failed to convert timestamp" << std::endl;
  }
  char buffer[20];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
  printf("First tick: ask=%.4f, bid=%.4f timestamp=%s\n", ticks[0].ask, ticks[0].bid, buffer);

  auto tradeManager = TradeManager::getInstance();

  std::string tradeId = tradeManager->openTrade(ticks[0].ask, 100000, true);
  std::cout << "Opened trade: " << tradeId << std::endl;

  size_t openTrades = tradeManager->reviewAccount();
  std::cout << "Number of open trades: " << openTrades << std::endl;

  bool closed = tradeManager->closeTrade(tradeId);
  std::cout << "Trade closed: " << (closed ? "yes" : "no") << std::endl;

  return 0;
  
}
