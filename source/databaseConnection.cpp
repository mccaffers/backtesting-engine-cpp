// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "databaseConnection.hpp"
#include <pqxx/pqxx>

DatabaseConnection::DatabaseConnection(const std::string& endpoint, int port,
                                     const std::string& dbname, const std::string& user,
                                     const std::string& password) {
  connection_string = std::format("host={} port={} dbname={} user={} password={} connect_timeout=3",
      endpoint, port, dbname, user, password);
}

void DatabaseConnection::executeQuery(const std::string& query) const {
  try {
      // Establish connection
      pqxx::connection conn(this->connection_string);

      // Rest of your code remains the same
      if (!conn.is_open()) {
          throw std::invalid_argument("Failed to open database connection");
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

  } catch (const pqxx::broken_connection& e) {
      std::cerr << "Connection error: " << e.what() << std::endl;
  } catch (const pqxx::sql_error& e) {
      std::cerr << "SQL error: " << e.what() << std::endl;
      std::cerr << "Query was: " << e.query() << std::endl;
  } catch (const pqxx::usage_error& e) {
      std::cerr << "Usage error: " << e.what() << std::endl;
  }
}
