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

    // Composite performance score and its components (see ResultsSummary).
    // The score blends expectancy/SQN, Calmar, and a trades-per-year
    // confidence multiplier so a sweep can be ranked by a single number.
    // All default to 0 so a zero-trade (or unscoreable) run serialises cleanly.
    // Units note: PnL is pip-denominated and STARTING_BALANCE is currency, so
    // the percentages are pip-relative proxies — consistent for ranking, not
    // an absolute account return.
    boost::decimal::decimal64_t performanceScore{0};
    boost::decimal::decimal64_t winRate{0};
    boost::decimal::decimal64_t tradeRatio{0};
    boost::decimal::decimal64_t expectancyScore{0};
    boost::decimal::decimal64_t calmarScore{0};
    boost::decimal::decimal64_t confidenceMultiplier{0};
    boost::decimal::decimal64_t maxDrawdownPercent{0};
};

// Wire shape pushed to Elasticsearch: the input Configuration plus the run's
// output stats. Serialises the timestamp as `@timestamp` for Kibana.
struct TradingResults {
    std::string RUN_ID;
    std::string timestamp;
    double durationSeconds;          // wall-clock seconds for this run's backtest
    std::string hostname;            // machine that ran the backtest
    tradingDefinitions::Configuration config;
    TradingResultsStats results;

    static std::string nowIsoUtc();
};

// Wire shape for a run that was cut off early (e.g. it breached the account
// loss limit). Carries the stats as they stood at the cutoff so a failed run
// is still fully inspectable in Kibana. `reason` is a STATIC, low-cardinality
// string (it becomes reason.keyword — one distinct value per failure class, so
// terms aggregations work); the run-specific numbers travel in the dedicated
// numeric fields below, never interpolated into the string.
struct TradingFailure {
    std::string RUN_ID;
    std::string timestamp;
    double durationSeconds;          // wall-clock seconds until the cutoff
    std::string reason;              // static failure class, aggregatable
    double breachPnlPips;            // account PnL at cutoff, pips of price movement
    double lossFloorPips;            // the configured pip budget that was breached
    std::string hostname;            // machine that ran the backtest
    tradingDefinitions::Configuration config;
    TradingResultsStats results;
};

// Compact terminal record emitted once per run, regardless of how it ended:
// the input Configuration plus the bare outcome flag, how long it took, and the
// host that produced it. Unlike TradingResults/TradingFailure it carries no
// per-trade stats — it is the at-a-glance "this run finished" signal. Lands in
// the weekly outcome_index::kFinalBase index.
struct TradeFinal {
    std::string RUN_ID;
    std::string timestamp;
    double durationSeconds;          // wall-clock seconds for this run
    // 1 = performance gate cleared — the same population the results/winners
    // indices receive; 0 = underperformed or loss-limit cutoff (see `status`).
    int success;
    // Terminal state as a keyword: "completed" (performance gate cleared —
    // the run also reports to the results/winners index and may chain),
    // "underperformed"
    // (finished its ticks but failed the gate; this record is its only
    // report), or "loss_limit_breached". Splits the success=0 class.
    std::string status;
    std::string hostname;            // machine that ran the backtest
    tradingDefinitions::Configuration config;

    // Host this process is running on ("unknown" if it can't be resolved).
    // Lives here (a normal TU) rather than in the import-std module that calls
    // it, since it needs the POSIX gethostname() header.
    static std::string localHostname();
};

void to_json(nlohmann::json& j, const TradingResultsStats& s);
void to_json(nlohmann::json& j, const TradingResults& r);
void to_json(nlohmann::json& j, const TradingFailure& f);
void to_json(nlohmann::json& j, const TradeFinal& f);
