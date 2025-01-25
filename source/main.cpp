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

// external headers
#include <nlohmann/json.hpp>

// backtesting engine headers
#include "configManager.hpp"
#include "serviceA.hpp"
#include "databaseConnection.hpp"
#include "base64.hpp"
#include "trading_definitions.hpp"  // For everything

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

  // DatabaseConnection Constructor
  // const std::string& endpoint
  // int port
  // const std::string& dbname
  // const std::string& user
  // const std::string& password
  DatabaseConnection db(argv[1], 8812, "qdb", "admin", "quest");

  // Load strategy from Base64 argv[2]
  parseJson(argv[2]);

  // Example query - replace with your actual query
  std::string query = "SELECT * FROM EURUSD LIMIT 5;";

  db.executeQuery(query);

return 0;
  
}
