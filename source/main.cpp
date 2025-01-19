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
#include "application.hpp"
#include "serviceA.hpp"
#include "databaseConnection.hpp"
#include "base64.hpp"
#include "trading_definitions.hpp"  // For everything

using json = nlohmann::json;

int main(int argc, const char * argv[]) {
  
    DatabaseConnection db(argv[1]);
  
    // Example query - replace with your actual query
    std::string query = "SELECT * FROM EURUSD LIMIT 5;";
  
    db.executeQuery(query);
  
    return 0;
  
}

int parseJson( const char * argv[]) {
  // Ingest parameters
  std::string output = Base64::b64decode(argv[2]);
  
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
  
//  std::cout << j;
  auto config = j.get<trading_definitions::Configuration>();
  std::cout << config.RUN_ID << std::endl;
  
//  std::string value = j["LAST_MONTHS"];
  
  // Throughput of tick data
  // Strategy makes a decision, returns trade YES/NO
  // Perform analysis of open trades against tick data
  

  return 0;
}

//  std::cout << "Hello, World!\n";
//  std::vector<int> numbers = {-1, -2, 3, 4, -5};
//  std::cout << Application().addNumbers(numbers) << std::endl;
//  
//  auto configManager = ConfigManager::getInstance();
//  
//  ServiceA serviceA(configManager);
//
//  serviceA.doSomething();
//
//  configManager->setConfig("New Configuration");
//
//  serviceA.doSomething();
//  

//}



