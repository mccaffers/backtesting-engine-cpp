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
    static std::vector<PriceData> streamPriceData(const DatabaseConnection& db, int LAST_MONTHS = 1);
    static std::string getBaseQuery(int LAST_MONTHS = 1);
private:
    static constexpr int STREAM_LIMIT = 200000;
};
