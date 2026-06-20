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

#include "shared/utilities/decimalJson.hpp"
#include "shared/tradingDefinitions.hpp"

// Per-run summary mirroring what ResultsSummary::summarise prints today.
struct TradingResultsStats {
    boost::decimal::decimal64_t finalPnl;
    std::size_t tradesOpened = 0;
    std::size_t tradesClosed = 0;
    std::size_t openedLong = 0;
    std::size_t openedShort = 0;
    std::size_t closedLong = 0;
    std::size_t closedShort = 0;
    std::size_t winners = 0;
    std::size_t losers = 0;
    std::size_t breakeven = 0;
    // Closes forced by the account loss limit, so winners/losers/avgPnl can
    // be read net of liquidation noise.
    std::size_t liquidated = 0;
    std::optional<boost::decimal::decimal64_t> avgPnl;
};

// Wire shape pushed to Elasticsearch: the input Configuration plus the run's
// output stats. Serialises the timestamp as `@timestamp` for Kibana.
struct TradingResults {
    std::string RUN_ID;
    std::string timestamp;
    double durationSeconds;          // wall-clock seconds for this run's backtest
    tradingDefinitions::Configuration config;
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
    tradingDefinitions::Configuration config;
    TradingResultsStats results;
};

void to_json(nlohmann::json& j, const TradingResultsStats& s);
void to_json(nlohmann::json& j, const TradingResults& r);
void to_json(nlohmann::json& j, const TradingFailure& f);
