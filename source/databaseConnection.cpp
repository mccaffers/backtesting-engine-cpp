// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "databaseConnection.hpp"
#include "base64.hpp"
#include <pqxx/pqxx>

DatabaseConnection::DatabaseConnection(const std::string& endpoint, int port,
                                     const std::string& dbname, const std::string& user,
                                     const std::string& password) {
    connection_string =
        "host=" + endpoint + " "
        "port=" + std::to_string(port) + " "
        "dbname=" + dbname + " "
        "user=" + user + " "
        "password=" + password + " "
        "connect_timeout=3";

}

std::vector<PriceData> DatabaseConnection::executeQuery(const std::string& query) const {
    std::vector<PriceData> results;
    
    try {
        pqxx::connection conn(this->connection_string);

        if (!conn.is_open()) {
            throw std::invalid_argument("Failed to open database connection");
        }

        std::cout << "Connected to database successfully!" << std::endl;

        pqxx::work txn(conn);
        pqxx::result result = txn.exec(query);

        // Convert results to PriceData objects
        for (const auto& row : result) {
            double value1 = row[0].as<double>();
            double value2 = row[1].as<double>();
            std::string timestamp_str = row[2].as<std::string>();
            
            auto timestamp = Utilities::parseTimestamp(timestamp_str);
            
            results.emplace_back(value1, value2, timestamp);
        }

        txn.commit();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return results;
}

// Example usage function to demonstrate how to work with the results
void DatabaseConnection::printResults(const std::vector<PriceData>& results) const {
    for (const auto& data : results) {
        // Convert timestamp back to string for display
        auto time_t = std::chrono::system_clock::to_time_t(data.timestamp);
        struct tm tm = {};
        if (localtime_r(&time_t, &tm) == nullptr) {
            std::cerr << "Error: failed to convert timestamp" << std::endl;
            continue;
        }
        std::stringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        
        std::cout << std::fixed << std::setprecision(4)
                 << data.value1 << "\t"
                 << data.value2 << "\t"
                 << ss.str() << std::endl;
    }
}
