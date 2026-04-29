// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#include "sqlManager.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

std::vector<PriceData> SqlManager::streamPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS) {
    if (symbols.empty()) {
        return {};
    }

    std::ostringstream query;
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            query << " UNION ALL ";
        }
        query << "SELECT '" << symbols[i] << "' as symbol, * FROM '" << symbols[i]
              << "' WHERE timestamp >= dateadd('M', -" << LAST_MONTHS << ", now())";
    }
    query << " ORDER BY timestamp";

    std::cout << "Executing query: " << query.str() << std::endl;
    return db.streamQuery(query.str());
}
