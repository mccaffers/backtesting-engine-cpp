// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
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

using json = nlohmann::json;

int parseJson(const std::string& input) {
  
  // Ingest parameters
  std::string output = Base64::b64decode(input);
  
  // Debug, console print out
  std::cout << output;
  
  json j;
  try
  {
      j = json::parse(output);
  }
  catch (json::parse_error& ex)
  {
      std::cerr << "parse error at byte " << ex.byte << std::endl;
  }
  
  auto config = j.get<trading_definitions::Configuration>();
  std::cout << config.RUN_ID << std::endl;

  return 0;
}

int main(int argc, const char * argv[]) {
  
  // Connect to QuestDb argv[1]
  DatabaseConnection db(argv[1], 8812, "qdb", "admin", "quest");

  // Load strategy from Base64 argv[2]
  parseJson(argv[2]);

  // Example query - replace with your actual query
  std::string query = "SELECT * FROM EURUSD LIMIT 5;";

  std::vector<PriceData> priceData = db.executeQuery(query);
  
  // Convert timestamp to readable format for debugging
  auto timeT = std::chrono::system_clock::to_time_t(priceData[0].timestamp);
  std::cout << "Timestamp: " << std::put_time(std::localtime(&timeT), "%Y-%m-%d %H:%M:%S") << std::endl;

  auto tradeManager = TradeManager::getInstance();

  // Open a trade
  std::string tradeId = tradeManager->openTrade(1.2345, 100000, true);
  std::cout << "Opened trade: " << tradeId << std::endl;

  // Review account
  size_t openTrades = tradeManager->reviewAccount();
  std::cout << "Number of open trades: " << openTrades << std::endl;

  // Close trade
  bool closed = tradeManager->closeTrade(tradeId);
  std::cout << "Trade closed: " << (closed ? "yes" : "no") << std::endl;

  return 0;
  
}
