// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <boost/decimal.hpp>
#include <nlohmann/json.hpp>

#include "utilities/decimal_json.hpp"
#include "trading_definitions.hpp"

// Per-run summary mirroring what Reporting::summarise prints today.
struct TradingResultsStats {
    boost::decimal::decimal64_t finalPnl;
    std::size_t tradesOpened;
    std::size_t tradesClosed;
    std::size_t openedLong;
    std::size_t openedShort;
    std::size_t closedLong;
    std::size_t closedShort;
    std::size_t winners;
    std::size_t losers;
    std::size_t breakeven;
    std::optional<boost::decimal::decimal64_t> avgPnl;
};

// Wire shape pushed to Elasticsearch: the input Configuration plus the run's
// output stats. Serialises the timestamp as `@timestamp` for Kibana.
struct TradingResults {
    std::string RUN_ID;
    std::string timestamp;
    trading_definitions::Configuration config;
    TradingResultsStats results;

    static std::string nowIsoUtc();
};

void to_json(nlohmann::json& j, const TradingResultsStats& s);
void to_json(nlohmann::json& j, const TradingResults& r);
