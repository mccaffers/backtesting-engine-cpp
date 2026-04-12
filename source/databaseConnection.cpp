// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "databaseConnection.hpp"
#include "base64.hpp"
#include <pqxx/pqxx>
#include <cstdio>
#include <charconv>
#include <execution>
#include <algorithm>

static std::chrono::system_clock::time_point fastParseTimestamp(const char* ts) {
    int year, month, day, hour, min, sec, usec = 0;
    std::sscanf(ts, "%4d-%2d-%2d %2d:%2d:%2d.%d", &year, &month, &day, &hour, &min, &sec, &usec);

    // Cache timegm per date — tick data is time-ordered so date changes rarely
    static char cachedDate[11] = {};
    static time_t cachedEpoch = 0;
    if (std::memcmp(ts, cachedDate, 10) != 0) {
        std::memcpy(cachedDate, ts, 10);
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon  = month - 1;
        tm.tm_mday = day;
        tm.tm_isdst = 0;
        cachedEpoch = timegm(&tm);
    }

    time_t t = cachedEpoch + hour * 3600 + min * 60 + sec;
    return std::chrono::system_clock::from_time_t(t) + std::chrono::microseconds(usec);
}

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

std::vector<PriceData> DatabaseConnection::streamQuery(const std::string& query) const {
    pqxx::connection conn(this->connection_string);
    pqxx::nontransaction txn(conn);
    pqxx::result result = txn.exec(query);

    std::vector<PriceData> results(result.size());

    for (int i = 0; i < (int)result.size(); ++i) {
        const auto& row = result[i];
        double ask, bid;
        auto symbol = row[0].view();
        auto sv1 = row[1].view();
        auto sv2 = row[2].view();
        std::from_chars(sv1.data(), sv1.data() + sv1.size(), ask);
        std::from_chars(sv2.data(), sv2.data() + sv2.size(), bid);
        results[i] = PriceData(ask, bid, fastParseTimestamp(row[3].c_str()), std::string(symbol));
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
                 << data.ask << "\t"
                 << data.bid << "\t"
                 << ss.str() << std::endl;
    }
}
