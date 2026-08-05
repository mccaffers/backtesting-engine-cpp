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
    // The window is LAST_MONTHS long and ends OFFSET_MONTHS before now, so
    // OFFSET_MONTHS = 0 loads the most recent data and e.g. (6, 12) loads the
    // 6-month window that ended a year ago.
    static std::vector<PriceData> loadPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS = 1, int OFFSET_MONTHS = 0);

    // Month boundaries for a tick superset, computed BY QUESTDB so they carry
    // its exact dateadd('M', ...) semantics (month-end day clamping included) —
    // never reimplemented locally. One statement evaluates now() once, so every
    // column shares a single snapshot instant T: index 0 is T itself and index
    // m is m calendar months before T. loadMonthBoundaries returns exactly
    // months+1 instants.
    static std::string buildMonthBoundariesQuery(int months);
    static std::vector<std::chrono::system_clock::time_point> loadMonthBoundaries(
        const DatabaseConnection& db, int months);

    // Tick load over an explicit [lower, upper) window, passed as literal
    // timestamps rather than now()-relative dateadd expressions — so a superset
    // query and the boundary row it was sliced from can never disagree about
    // where a month starts.
    static std::string buildPriceDataBetweenQuery(
        const std::vector<std::string>& symbols,
        std::chrono::system_clock::time_point lower,
        std::chrono::system_clock::time_point upper);
    static std::vector<PriceData> loadPriceDataBetween(
        const DatabaseConnection& db,
        const std::vector<std::string>& symbols,
        std::chrono::system_clock::time_point lower,
        std::chrono::system_clock::time_point upper);

    // ISO-8601 with full microseconds ("2026-07-07T14:03:12.123456Z"): valid as
    // a QuestDB timestamp literal, and round-trips exactly through
    // fastParseTimestamp so boundary instants survive format -> parse unchanged.
    static std::string formatTimestamp(std::chrono::system_clock::time_point tp);

private:
    // Symbols arrive via Redis payloads and are interpolated into query text,
    // so only names from the canonical table are accepted. This blocks SQL
    // injection and catches typos before they become QuestDB errors.
    static void validateSymbols(const std::vector<std::string>& symbols);

    // Shared SELECT ... UNION ALL ... shape of both tick loads; the caller
    // supplies the per-symbol WHERE clause. Ends with ORDER BY timestamp, plus
    // a `, symbol` tie-break only for multi-symbol queries: equal-timestamp
    // ticks across symbols otherwise come back in whatever order QuestDB
    // merges them, while the tie-break on a single-symbol query would defeat
    // the elided sort on the designated timestamp for no gain.
    static std::string buildTickQuery(const std::vector<std::string>& symbols,
                                      const std::string& whereClause);
};

void SqlManager::validateSymbols(const std::vector<std::string>& symbols) {
    for (const auto& symbol : symbols) {
        if (symbol_scale::get(symbol) == symbol_scale::kUnknown) {
            throw std::invalid_argument("Unknown symbol rejected: " + symbol);
        }
    }
}

std::string SqlManager::buildTickQuery(const std::vector<std::string>& symbols,
                                       const std::string& whereClause) {
    // Columns are named explicitly (never `*`): executeQuery reads results
    // positionally as (symbol, ask, bid, timestamp), so a tick table whose
    // physical column order differs — or ever gains a column — must not be
    // able to silently shift prices into the wrong fields.
    std::ostringstream query;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            query << " UNION ALL ";
        }
        query << "SELECT '" << symbols[i] << "' as symbol, ask, bid, timestamp FROM '"
              << symbols[i] << "' WHERE " << whereClause;
    }
    query << " ORDER BY timestamp";
    if (symbols.size() > 1) {
        query << ", symbol";
    }
    return query.str();
}

std::vector<PriceData> SqlManager::loadPriceData(const DatabaseConnection& db, const std::vector<std::string>& symbols, int LAST_MONTHS, int OFFSET_MONTHS) {
    if (symbols.empty()) {
        return {};
    }

    validateSymbols(symbols);

    std::ostringstream where;
    where << "timestamp >= dateadd('M', -" << (LAST_MONTHS + OFFSET_MONTHS) << ", now())";
    // Only cap the window when it is actually shifted back — with a zero
    // offset the upper bound would just be now(), so leave it open and
    // keep the default query identical to what it always was.
    if (OFFSET_MONTHS > 0) {
        where << " AND timestamp < dateadd('M', -" << OFFSET_MONTHS << ", now())";
    }
    const std::string query = buildTickQuery(symbols, where.str());

    std::cout << "Executing query: " << query << std::endl;
    return db.executeQuery(query);
}

std::string SqlManager::formatTimestamp(const std::chrono::system_clock::time_point tp) {
    // %S on a microseconds-precision time prints the full fraction
    // ("12.123456"), which fastParseTimestamp reads back to the same
    // microsecond — the round-trip the boundary handling relies on.
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z",
                       std::chrono::floor<std::chrono::microseconds>(tp));
}

std::string SqlManager::buildMonthBoundariesQuery(const int months) {
    if (months < 1) {
        throw std::invalid_argument("Month boundaries need at least one month back");
    }
    std::ostringstream query;
    query << "SELECT now()";
    for (int m = 1; m <= months; ++m) {
        query << ", dateadd('M', -" << m << ", now())";
    }
    return query.str();
}

std::vector<std::chrono::system_clock::time_point> SqlManager::loadMonthBoundaries(
    const DatabaseConnection& db, const int months) {
    auto boundaries = db.queryTimestampRow(buildMonthBoundariesQuery(months));
    if (boundaries.size() != static_cast<std::size_t>(months) + 1) {
        throw std::runtime_error("Month boundary row returned " +
                                 std::to_string(boundaries.size()) + " columns, expected " +
                                 std::to_string(months + 1));
    }
    return boundaries;
}

std::string SqlManager::buildPriceDataBetweenQuery(
    const std::vector<std::string>& symbols,
    const std::chrono::system_clock::time_point lower,
    const std::chrono::system_clock::time_point upper) {
    std::ostringstream where;
    where << "timestamp >= cast('" << formatTimestamp(lower)
          << "' as timestamp) AND timestamp < cast('" << formatTimestamp(upper)
          << "' as timestamp)";
    return buildTickQuery(symbols, where.str());
}

std::vector<PriceData> SqlManager::loadPriceDataBetween(
    const DatabaseConnection& db,
    const std::vector<std::string>& symbols,
    const std::chrono::system_clock::time_point lower,
    const std::chrono::system_clock::time_point upper) {
    if (symbols.empty()) {
        return {};
    }

    validateSymbols(symbols);

    const std::string query = buildPriceDataBetweenQuery(symbols, lower, upper);
    std::cout << "Executing query: " << query << std::endl;
    return db.executeQuery(query);
}
