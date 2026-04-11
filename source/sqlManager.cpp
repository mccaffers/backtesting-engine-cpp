// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#include "sqlManager.hpp"
#include <string>
#include <vector>

std::string SqlManager::getBaseQuery(int LAST_MONTHS) {
    return "SELECT * FROM EURUSD WHERE timestamp >= dateadd('M', -" + std::to_string(LAST_MONTHS) + ", now()) LIMIT " + std::to_string(STREAM_LIMIT);
}

std::vector<PriceData> SqlManager::streamPriceData(const DatabaseConnection& db, int LAST_MONTHS) {
    std::string query = getBaseQuery(LAST_MONTHS);
    std::cout << "Executing query: " << query << std::endl;
    return db.streamQuery(query);
}
