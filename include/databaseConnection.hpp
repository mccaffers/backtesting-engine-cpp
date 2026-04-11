// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <iostream>
#include <pqxx/pqxx>
#include "models/priceData.hpp"

class DatabaseConnection {
private:
  std::string connection_string;
  
public:
  DatabaseConnection(const std::string& endpoint = "localhost",
                        int port = 8812,
                        const std::string& dbname = "qdb",
                        const std::string& user = "admin",
                        const std::string& password = "");
  
  void printResults(const std::vector<PriceData>& results) const;
  std::vector<PriceData> executeQuery(const std::string& query) const;
  std::vector<PriceData> streamQuery(const std::string& query) const;
  
  const std::string& getConnectionString() const {
    return connection_string;
  }

};
