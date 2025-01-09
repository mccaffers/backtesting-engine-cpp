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

std::string checkInput(std::string base64_input){
       // Remove any whitespace from the input
      base64_input.erase(
          std::remove_if(base64_input.begin(), base64_input.end(), ::isspace),
          base64_input.end()
      );
  
  return base64_input;
};

bool isValidBase64(const std::string& input) {
    // Valid base64 characters
    const char* valid_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    
    // Check if string length is valid (multiple of 4)
    if (input.length() % 4 != 0) {
        return false;
    }
    
    // Check if all characters are valid base64 characters
    return std::all_of(input.begin(), input.end(),
        [](char c) {
            const char* valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
            return strchr(valid, c) != nullptr;
        });
}

int main(int argc, const char * argv[]) {
  
  // Throughput of tick data
  // Strategy makes a decision, returns trade YES/NO
  // Perform analysis of open trades against tick data
  
  std::string output = checkInput(argv[1]);
  bool valid = isValidBase64(output);
  
//  std::cout << valid;
  std::cout << Base64().b64decode(output);

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



