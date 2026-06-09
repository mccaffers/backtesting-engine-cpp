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
    // Closes forced by the account loss limit, so winners/losers/avgPnl can
    // be read net of liquidation noise.
    std::size_t liquidated;
    std::optional<boost::decimal::decimal64_t> avgPnl;
};

// Wire shape pushed to Elasticsearch: the input Configuration plus the run's
// output stats. Serialises the timestamp as `@timestamp` for Kibana.
struct TradingResults {
    std::string RUN_ID;
    std::string timestamp;
    double durationSeconds;          // wall-clock seconds for this run's backtest
    trading_definitions::Configuration config;
    TradingResultsStats results;

    static std::string nowIsoUtc();
};

// Wire shape for a run that was cut off early (e.g. it breached the account
// loss limit). Carries the stats as they stood at the cutoff so a failed run
// is still fully inspectable in Kibana; `reason` says why it was stopped.
struct TradingFailure {
    std::string RUN_ID;
    std::string timestamp;
    double durationSeconds;          // wall-clock seconds until the cutoff
    std::string reason;
    trading_definitions::Configuration config;
    TradingResultsStats results;
};

void to_json(nlohmann::json& j, const TradingResultsStats& s);
void to_json(nlohmann::json& j, const TradingResults& r);
void to_json(nlohmann::json& j, const TradingFailure& f);
