// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#include "sqlManager.hpp"
#include <string>
#include <vector>

std::string SqlManager::getBaseQuery() {
    return "SELECT * FROM EURUSD WHERE timestamp >= dateadd('M', -" + std::to_string(LAST_MONTHS) + ", now()) LIMIT 40000000";
}

std::vector<PriceData> SqlManager::streamPriceData(const DatabaseConnection& db) {
    std::string query = getBaseQuery();
    std::cout << "Executing query: " << query << std::endl;
    return db.streamQuery(query);
}
