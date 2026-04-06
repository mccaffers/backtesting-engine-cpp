// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#include "sqlManager.hpp"

std::string SqlManager::getBaseQuery() {
    return "SELECT * FROM EURUSD LIMIT " + std::to_string(DEFAULT_LIMIT) + ";";
}

std::vector<PriceData> SqlManager::getInitialPriceData(const DatabaseConnection& db) {
    return db.executeQuery(getBaseQuery());
}
