// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "databaseConnection.hpp"
#include <pqxx/pqxx>

void DatabaseConnection::executeQuery(const std::string& query) {
  try {
      // Establish connection
      pqxx::connection conn(this->connection_string);

      // Rest of your code remains the same
      if (!conn.is_open()) {
          throw std::runtime_error("Failed to open database connection");
      }

      std::cout << "Connected to database successfully!" << std::endl;

      // Create a transaction
      pqxx::work txn(conn);

      // Execute query
      pqxx::result result = txn.exec(query);

      // Print results
      for (const auto& row : result) {
          for (const auto& field : row) {
              std::cout << field.c_str() << "\t";
          }
          std::cout << std::endl;
      }

      // Commit transaction
      txn.commit();

  } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << std::endl;
  }
}
