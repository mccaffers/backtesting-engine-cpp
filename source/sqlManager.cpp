// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#include "sqlManager.hpp"
#include <iostream>
#include <string>
#include <vector>

std::string SqlManager::getBaseQuery(const std::vector<std::string>& symbols, int LAST_MONTHS) {
    if (symbols.empty()) {
        return "";
    }
    
    std::string query;
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            query += " UNION ALL ";
        }
        query += "SELECT '" + symbols[i] + "' as symbol, * FROM '" + symbols[i] + "' WHERE timestamp >= dateadd('M', -" + std::to_string(LAST_MONTHS) + ", now())";
    }
    query += " ORDER BY timestamp";
    
    return query;
}

std::vector<PriceData> SqlManager::streamPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS) {
    std::string query = getBaseQuery(symbols, LAST_MONTHS);
    std::cout << "Executing query: " << query << std::endl;
    return db.streamQuery(query);
}
