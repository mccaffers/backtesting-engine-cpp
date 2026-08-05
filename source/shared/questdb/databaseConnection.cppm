// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <ctime>  // POSIX timegm (not exported by `import std`)

#include <pqxx/pqxx>

export module databaseConnection;

import std;         // replaces <charconv>, <cstdint>, <cstdio>, <format>, <stdexcept>,
                    // <chrono>, <string>, <string_view>, <vector>
import ohlcObject;  // OhlcObject (queryOhlc result rows)
import priceData;   // PriceData

export class DatabaseConnection {
private:
  std::string connection_string;

public:
  DatabaseConnection(const std::string& endpoint = "localhost",
                        int port = 8812,
                        const std::string& dbname = "qdb",
                        const std::string& user = "admin",
                        const std::string& password = "");

  std::vector<PriceData> executeQuery(const std::string& query) const;

  std::vector<OhlcObject> queryOhlc(const std::string& query) const;

  // Runs a query expected to return exactly ONE row whose columns are all
  // timestamps (e.g. the month-boundary row) and parses each column with
  // fastParseTimestamp. Throws std::runtime_error on any other row count so a
  // malformed boundary query can never be misread as data.
  std::vector<std::chrono::system_clock::time_point> queryTimestampRow(
      const std::string& query) const;

  const std::string& getConnectionString() const {
    return connection_string;
  }

};

export class InvalidTimestampFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Caches timegm per date — tick data is time-ordered so the date changes
// rarely. The caller owns the cache, so each thread/query gets its own and
// the parse stays safe if loading ever moves onto the ThreadPool.
// Exported (with the parser below) so the fractional-seconds handling is
// directly unit-testable.
export struct DateCache {
    std::string date;
    std::time_t epoch = 0;
};

export std::chrono::system_clock::time_point fastParseTimestamp(const char* ts, DateCache& cache) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    int consumed = 0;
    // %*1[T ] accepts either separator between date and time: pgwire text
    // format uses a space, ISO-8601 literals (SqlManager::formatTimestamp)
    // use 'T'. Assignment-suppressed, so parsedFields still counts 6.
    const int parsedFields =
        std::sscanf(ts, "%4d-%2d-%2d%*1[T ]%2d:%2d:%2d%n", &year, &month, &day, &hour, &min, &sec, &consumed);
    if (parsedFields != 6) {
        throw InvalidTimestampFormatError("Invalid timestamp format: " + std::string(ts));
    }

    // Fractional seconds are scaled by the number of digits actually present:
    // Postgres wire-format text trims trailing zeros, so ".5" means 500000 µs —
    // reading the digits as a plain integer (the old "%d" parse) would have
    // turned it into 5 µs, a 100000x error that corrupts tick ordering at bar
    // boundaries. Digits beyond microsecond precision are truncated.
    std::int64_t usec = 0;
    if (ts[consumed] == '.') {
        const char* p = ts + consumed + 1;
        int digits = 0;
        while (digits < 6 && p[digits] >= '0' && p[digits] <= '9') {
            usec = usec * 10 + (p[digits] - '0');
            ++digits;
        }
        if (digits == 0) {
            throw InvalidTimestampFormatError(
                "Invalid fractional seconds in timestamp: " + std::string(ts));
        }
        for (; digits < 6; ++digits) {
            usec *= 10;
        }
    }

    const std::string_view date(ts, 10);
    if (cache.date != date) {
        cache.date.assign(date);
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon  = month - 1;
        tm.tm_mday = day;
        tm.tm_isdst = 0;
        cache.epoch = timegm(&tm);
    }

    std::time_t t = cache.epoch + hour * 3600 + min * 60 + sec;
    return std::chrono::system_clock::from_time_t(t) + std::chrono::microseconds(usec);
}

DatabaseConnection::DatabaseConnection(const std::string& endpoint, int port,
                                     const std::string& dbname, const std::string& user,
                                     const std::string& password) {
    connection_string = std::format(
        "host={} port={} dbname={} user={} password={} connect_timeout=3",
        endpoint, port, dbname, user, password);

}

std::vector<PriceData> DatabaseConnection::executeQuery(const std::string& query) const {
    pqxx::connection conn(this->connection_string);
    pqxx::nontransaction txn(conn);
    pqxx::result result = txn.exec(query);

    std::vector<PriceData> results(result.size());
    DateCache dateCache;

    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& row = result[static_cast<pqxx::result::size_type>(i)];
        std::int32_t ask = 0;
        std::int32_t bid = 0;
        auto symbol = row[0].view();
        auto sv1 = row[1].view();
        auto sv2 = row[2].view();
        // ask/bid are INT32 scaled fixed-point in QuestDB now, so a plain
        // integer parse replaces the decimal from_chars — no decimal arithmetic
        // touches the load path.
        const auto askResult = std::from_chars(sv1.data(), sv1.data() + sv1.size(), ask);
        const auto bidResult = std::from_chars(sv2.data(), sv2.data() + sv2.size(), bid);
        if (askResult.ec != std::errc{} || bidResult.ec != std::errc{}) {
            throw std::runtime_error(std::format(
                "Failed to parse price for {}: ask='{}' bid='{}'", symbol, sv1, sv2));
        }
        results[i] = PriceData(ask, bid, fastParseTimestamp(row[3].c_str(), dateCache), std::string(symbol));
    }

    return results;
}

std::vector<std::chrono::system_clock::time_point> DatabaseConnection::queryTimestampRow(
    const std::string& query) const {
    pqxx::connection conn(this->connection_string);
    pqxx::nontransaction txn(conn);
    pqxx::result result = txn.exec(query);

    if (result.size() != 1) {
        throw std::runtime_error(std::format(
            "Timestamp-row query returned {} rows, expected exactly 1", result.size()));
    }

    const auto& row = result[0];
    std::vector<std::chrono::system_clock::time_point> timestamps(row.size());
    DateCache dateCache;
    for (pqxx::row::size_type i = 0; i < row.size(); ++i) {
        timestamps[i] = fastParseTimestamp(row[i].c_str(), dateCache);
    }
    return timestamps;
}

// Maps rows of (timestamp, open, high, low, close) — the shape produced by a
// SAMPLE BY aggregation over a tick table — onto OhlcObject. Prices carry the
// same scaled INT32 fixed-point as the underlying ask/bid columns. Bars come
// back in delivered order, marked complete; ordering is the caller's business.
std::vector<OhlcObject> DatabaseConnection::queryOhlc(const std::string& query) const {
    pqxx::connection conn(this->connection_string);
    pqxx::nontransaction txn(conn);
    pqxx::result result = txn.exec(query);

    std::vector<OhlcObject> bars(result.size());
    DateCache dateCache;

    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& row = result[static_cast<pqxx::result::size_type>(i)];
        OhlcObject& bar = bars[i];
        bar.date = fastParseTimestamp(row[0].c_str(), dateCache);
        int col = 1;
        for (std::int32_t* field : {&bar.open, &bar.high, &bar.low, &bar.close}) {
            const auto sv = row[col].view();
            const auto parsed = std::from_chars(sv.data(), sv.data() + sv.size(), *field);
            if (parsed.ec != std::errc{}) {
                throw std::runtime_error(std::format(
                    "Failed to parse OHLC column {}: '{}'", col, sv));
            }
            ++col;
        }
        bar.complete = true;
    }

    return bars;
}
