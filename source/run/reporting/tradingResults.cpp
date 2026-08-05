// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/reporting/tradingResults.hpp"

#include <unistd.h>  // gethostname

#include "run/reporting/elasticPublisher.hpp"

std::string TradingResults::nowIsoUtc() {
    return elastic::nowIsoUtc();
}

std::string TradeFinal::localHostname() {
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0) {
        return "unknown";
    }
    // POSIX leaves truncation behaviour unspecified, so guarantee termination.
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// decimalToJsonNumber emits decimal64_t fields as JSON numbers (not the
// string-encoded form the shared adl_serializer uses) so Elasticsearch/Kibana
// types them numerically. These reporting structs are output-only — never parsed
// back — so the numeric form is safe here. See shared/utilities/decimalJson.hpp.
void to_json(nlohmann::json& j, const TradingResultsStats& s) {
    j = nlohmann::json{
        {"finalPnl", decimalToJsonNumber(s.finalPnl)},
        {"tradesOpened", s.tradesOpened},
        {"tradesClosed", s.tradesClosed},
        {"openedLong", s.openedLong},
        {"openedShort", s.openedShort},
        {"closedLong", s.closedLong},
        {"closedShort", s.closedShort},
        {"winners", s.winners},
        {"losers", s.losers},
        {"breakeven", s.breakeven},
        {"liquidated", s.liquidated},
        {"performanceScore", decimalToJsonNumber(s.performanceScore)},
        {"winRate", decimalToJsonNumber(s.winRate)},
        {"tradeRatio", decimalToJsonNumber(s.tradeRatio)},
        {"expectancyScore", decimalToJsonNumber(s.expectancyScore)},
        {"calmarScore", decimalToJsonNumber(s.calmarScore)},
        {"confidenceMultiplier", decimalToJsonNumber(s.confidenceMultiplier)},
        {"maxDrawdownPercent", decimalToJsonNumber(s.maxDrawdownPercent)},
    };
    if (s.avgPnl) {
        j["avgPnl"] = decimalToJsonNumber(*s.avgPnl);
    } else {
        j["avgPnl"] = nullptr;
    }
}

namespace {
// Reporting-only view of the run Configuration: starts from the shared serializer
// (which string-encodes decimals and the pip/size ints for the Redis sweep
// round-trip) and overwrites only those numeric fields with real JSON numbers, so
// Elasticsearch types them numerically. Reaching in by key keeps this robust if
// Configuration gains fields, and leaves the shared to_json untouched for Redis.
nlohmann::json reportConfigJson(const tradingDefinitions::Configuration& c) {
    nlohmann::json j = c;
    j["STARTING_BALANCE"] = decimalToJsonNumber(c.STARTING_BALANCE);
    j["MAX_LOSS_PERCENT"] = decimalToJsonNumber(c.MAX_LOSS_PERCENT);
    const auto& v = c.STRATEGY.TRADING_VARIABLES;
    auto& tv = j["STRATEGY"]["TRADING_VARIABLES"];
    tv["STOP_DISTANCE_IN_ATR"]  = v.STOP_DISTANCE_IN_ATR;
    tv["LIMIT_DISTANCE_IN_ATR"] = v.LIMIT_DISTANCE_IN_ATR;
    tv["TRADING_SIZE"]           = v.TRADING_SIZE;
    return j;
}

// Top-level copies of the batch identity (also present under config.*) so
// Kibana filters and aggregations don't have to reach into the config object.
// Omitted entirely for pre-batch configs — absent keys, not empty strings, so
// legacy documents keep their exact shape.
void appendBatchMetadata(nlohmann::json& j,
                         const tradingDefinitions::Configuration& c) {
    if (!c.EXECUTION_TS.empty()) {
        j["executionTimestamp"] = c.EXECUTION_TS;
    }
    if (!c.BATCH.empty()) {
        j["batch"] = c.BATCH;
    }
}
}  // namespace

void to_json(nlohmann::json& j, const TradingResults& r) {
    j = nlohmann::json{
        {"RUN_ID", r.RUN_ID},
        {"@timestamp", r.timestamp},
        {"durationSeconds", r.durationSeconds},
        {"hostname", r.hostname},
        {"config", reportConfigJson(r.config)},
        {"results", r.results},
    };
    appendBatchMetadata(j, r.config);
}

void to_json(nlohmann::json& j, const TradingFailure& f) {
    j = nlohmann::json{
        {"RUN_ID", f.RUN_ID},
        {"@timestamp", f.timestamp},
        {"durationSeconds", f.durationSeconds},
        {"reason", f.reason},
        {"breachPnlPips", f.breachPnlPips},
        {"lossFloorPips", f.lossFloorPips},
        {"hostname", f.hostname},
        {"config", reportConfigJson(f.config)},
        {"results", f.results},
    };
    appendBatchMetadata(j, f.config);
}

void to_json(nlohmann::json& j, const TradeFinal& f) {
    j = nlohmann::json{
        {"RUN_ID", f.RUN_ID},
        {"@timestamp", f.timestamp},
        {"durationSeconds", f.durationSeconds},
        {"success", f.success},
        {"status", f.status},
        {"hostname", f.hostname},
        {"config", reportConfigJson(f.config)},
    };
    appendBatchMetadata(j, f.config);
}
