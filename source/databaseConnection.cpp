// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "databaseConnection.hpp"
#include <pqxx/pqxx>
#include <cstdio>
#include <format>
#include <stdexcept>
#include <boost/decimal.hpp>

class InvalidTimestampFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Caches timegm per date — tick data is time-ordered so the date changes
// rarely. The caller owns the cache, so each thread/query gets its own and
// the parse stays safe if loading ever moves onto the ThreadPool.
struct DateCache {
    std::string date;
    time_t epoch = 0;
};

static std::chrono::system_clock::time_point fastParseTimestamp(const char* ts, DateCache& cache) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    int usec = 0;
    const int parsedFields =
        std::sscanf(ts, "%4d-%2d-%2d %2d:%2d:%2d.%d", &year, &month, &day, &hour, &min, &sec, &usec);
    if (parsedFields != 6 && parsedFields != 7) {
        throw InvalidTimestampFormatError("Invalid timestamp format: " + std::string(ts));
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

    time_t t = cache.epoch + hour * 3600 + min * 60 + sec;
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
        boost::decimal::decimal64_t ask;
        boost::decimal::decimal64_t bid;
        auto symbol = row[0].view();
        auto sv1 = row[1].view();
        auto sv2 = row[2].view();
        // boost::decimal ships its own from_chars overload — std::from_chars
        // doesn't know about decimal64_t.
        const auto askResult = boost::decimal::from_chars(sv1.data(), sv1.data() + sv1.size(), ask);
        const auto bidResult = boost::decimal::from_chars(sv2.data(), sv2.data() + sv2.size(), bid);
        if (askResult.ec != std::errc{} || bidResult.ec != std::errc{}) {
            throw std::runtime_error(std::format(
                "Failed to parse price for {}: ask='{}' bid='{}'", symbol, sv1, sv2));
        }
        results[i] = PriceData(ask, bid, fastParseTimestamp(row[3].c_str(), dateCache), std::string(symbol));
    }

    return results;
}
