// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <string>
#include <vector>
#include "models/priceData.hpp"
#include "databaseConnection.hpp"

class SqlManager {
public:
    static std::vector<PriceData> getInitialPriceData(const DatabaseConnection& db);
    static std::string getBaseQuery();
private:
    static constexpr int DEFAULT_LIMIT = 1000;
};
