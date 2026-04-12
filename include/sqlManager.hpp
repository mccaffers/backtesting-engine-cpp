// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <string>
#include <vector>
#include "models/priceData.hpp"
#include "databaseConnection.hpp"

class SqlManager {
public:
    static std::vector<PriceData> streamPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS = 1);
    static std::string getBaseQuery(const std::vector<std::string>& symbols, int LAST_MONTHS = 1);

};
