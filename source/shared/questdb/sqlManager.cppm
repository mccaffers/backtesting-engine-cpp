// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module sqlManager;

import std;                 // replaces <iostream>, <sstream>, <stdexcept>, <string>, <vector>
import priceData;           // PriceData
import databaseConnection;  // DatabaseConnection
import symbolScale;         // symbol_scale::get / kUnknown

export class SqlManager {
public:
    static std::vector<PriceData> loadPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS = 1);
};

std::vector<PriceData> SqlManager::loadPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS) {
    if (symbols.empty()) {
        return {};
    }

    // Symbols arrive via Redis payloads and are interpolated into the query
    // below, so only accept names from the canonical table. This blocks SQL
    // injection and catches typos before they become QuestDB errors.
    for (const auto& symbol : symbols) {
        if (symbol_scale::get(symbol) == symbol_scale::kUnknown) {
            throw std::invalid_argument("Unknown symbol rejected: " + symbol);
        }
    }

    std::ostringstream query;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            query << " UNION ALL ";
        }
        query << "SELECT '" << symbols[i] << "' as symbol, * FROM '" << symbols[i]
              << "' WHERE timestamp >= dateadd('M', -" << LAST_MONTHS << ", now())";
    }
    query << " ORDER BY timestamp";

    std::cout << "Executing query: " << query.str() << std::endl;
    return db.executeQuery(query.str());
}
