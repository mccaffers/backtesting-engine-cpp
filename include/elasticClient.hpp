// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include "tradingResults.hpp"

// Minimal Elasticsearch HTTP client — PUT-only, for indexing TradingResults.
// Host is read from $ELASTICSEARCH_URL (default http://localhost:9200);
// docs land in index "trading_results" with a freshly generated UUID per put.
class ElasticClient {
public:
    static int putTradingResults(const TradingResults& results);
};
