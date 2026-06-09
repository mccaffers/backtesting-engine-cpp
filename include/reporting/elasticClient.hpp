// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include "tradingResults.hpp"

// Minimal Elasticsearch HTTP client — PUT-only, for indexing run outcomes.
// Host is read from $ELASTIC_HOST (default http://localhost:9200) with optional
// HTTP basic auth from $ELASTIC_USER / $ELASTIC_USER_PASSWORD. Completed runs
// land in index "trading_results"; runs cut off early (loss limit) land in
// "trading_failures". Each doc gets a freshly generated UUID.
class ElasticClient {
public:
    static int putTradingResults(const TradingResults& results);
    static int putTradingFailure(const TradingFailure& failure);
};
