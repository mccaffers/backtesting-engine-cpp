// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <iostream>
#include <vector>
#include <memory>
#include "ConfigManager.hpp"
#include "Application.hpp"
#include "ServiceA.hpp"
#include "DatabaseConnection.hpp"
#include "Base64.hpp"
#include <string>
#include <json.hpp>



using json = nlohmann::json;

int main(int argc, const char * argv[]) {

  // Ingest parameters
  std::string output = Base64::b64decode(Base64::checkInput(argv[1]));
//  std::cout << Base64::b64decode(output);
  json j;
  try
  {
      j = json::parse(output);
  }
  catch (json::parse_error& ex)
  {
      std::cerr << "parse error at byte " << ex.byte << std::endl;
  }
  
  std::cout << j;
  
  
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
//  DatabaseConnection db;
//      
//  // Example query - replace with your actual query
//  std::string query = "SELECT * FROM EURUSD LIMIT 5;";
//      
//  db.executeQuery(query);
//  
//  return 0;
//}



