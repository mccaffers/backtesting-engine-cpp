// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <iostream>
#include <pqxx/pqxx>

class DatabaseConnection {
private:
  std::string connection_string;
  
public:
  DatabaseConnection() {
    // Convert your connection string format to libpqxx format
    connection_string =
    "host=100.85.44.114 "
    "port=8812 "
    "dbname=qdb "
    "user=admin "
    "password=quest "
    "connect_timeout=3";
  }
  
  void executeQuery(const std::string& query);

};
