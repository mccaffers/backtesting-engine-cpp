// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

// Wire shape for ONE closed trade, outcome_index::kTradesIndex — the opt-in
// per-trade companion to the per-run docs in tradingResults.hpp. Emitted only
// when $ELASTIC_TRADES_ENABLED=1, bulk-posted once at end of run. Plain header
// (not a module) for the same reason as tradingResults.hpp: nlohmann's ADL
// to_json does not resolve across module boundaries.
struct TradeDocument {
    std::string RUN_ID;
    // Serialised as `@timestamp` = SIMULATION closeTime, so Kibana plots
    // trades across the historical backtest window. A freshly-run backtest
    // therefore does NOT appear under "last 15 minutes" — widen the time range
    // to the backtest window, or query `ingestedAt` (wall clock) instead.
    std::string timestamp;
    std::string ingestedAt;          // wall clock when the batch was posted
    std::string hostname;            // machine that ran the backtest
    std::string strategyUuid;        // config.STRATEGY.UUID (one per param combo)
    std::string strategyName;        // config.STRATEGY.TRADING_VARIABLES.STRATEGY
    std::string tradeId;             // Trade.id ("T{n}"), unique per execution
    std::string symbol;
    std::string direction;           // "LONG"/"SHORT" — static, keyword-aggregatable
    std::int32_t size;
    // Real decimal prices: raw INT32 points / symbol_scale::getPriceScale().
    // When the symbol's scale is unknown (priceScale == 0 below) the raw
    // integer values are carried through unchanged.
    double entryPrice;
    double entryBid;
    double entryAsk;
    double closePrice;
    // Exit trigger levels. Only meaningful when the matching distance is
    // non-zero; serialised as JSON null when disarmed so Kibana doesn't plot
    // fake levels (see to_json).
    double stopPrice;
    double limitPrice;
    std::int32_t stopDistancePips;
    std::int32_t limitDistancePips;
    std::string openTime;            // sim time, ISO-8601 UTC with milliseconds
    std::string closeTime;           // sim time, ISO-8601 UTC with milliseconds
    double holdSeconds;              // sim closeTime - openTime
    std::int64_t pnlPoints;          // exact realized PnL (points x size)
    double pnlPips;                  // pnlPoints / (scalingFactor * size); 0 if unmeasurable
    bool liquidated;                 // forced close by loss limit vs organic
    int scalingFactor;               // points-per-pip (pip math recoverable)
    int priceScale;                  // raw->real divisor used; 0 = unknown, prices left raw

    // ISO-8601 UTC with milliseconds ("YYYY-MM-DDTHH:MM:SS.mmmZ"). Ticks are
    // sub-second, so the seconds-only stamps used elsewhere would collapse
    // trades onto the same instant in Kibana. Lives here (a normal TU) rather
    // than in an import-std module since it needs POSIX gmtime_r.
    static std::string isoUtcMillis(std::chrono::system_clock::time_point tp);
};

void to_json(nlohmann::json& j, const TradeDocument& t);
