// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"
#include "run/reporting/tradingResults.hpp"

export module elasticClient;

// Typed front-end over the shared Elasticsearch publisher (elastic::putDocument
// in shared/reporting). Completed runs land in index "trading_results"; runs
// cut off early (loss limit) land in "trading_failures". The transport, env
// config and auth all live in the shared publisher so the run path and the
// shared redis-consumer path report through one implementation.
export class ElasticClient {
public:
    static int putTradingResults(const TradingResults& results);
    static int putTradingFailure(const TradingFailure& failure);
    // Compact per-run terminal record; lands in index "trading_final".
    static int putTradeFinal(const TradeFinal& result);
};

int ElasticClient::putTradingResults(const TradingResults& results) {
    return elastic::putDocument("trading_results", nlohmann::json(results).dump());
}

int ElasticClient::putTradingFailure(const TradingFailure& failure) {
    return elastic::putDocument("trading_failures", nlohmann::json(failure).dump());
}

int ElasticClient::putTradeFinal(const TradeFinal& result) {
    return elastic::putDocument("trading_final", nlohmann::json(result).dump());
}
