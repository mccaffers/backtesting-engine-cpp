// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/reporting/tradingResults.hpp"

#include "shared/reporting/elasticPublisher.hpp"

std::string TradingResults::nowIsoUtc() {
    return elastic::nowIsoUtc();
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
    tv["STOP_DISTANCE_IN_PIPS"]  = v.STOP_DISTANCE_IN_PIPS;
    tv["LIMIT_DISTANCE_IN_PIPS"] = v.LIMIT_DISTANCE_IN_PIPS;
    tv["TRADING_SIZE"]           = v.TRADING_SIZE;
    return j;
}
}  // namespace

void to_json(nlohmann::json& j, const TradingResults& r) {
    j = nlohmann::json{
        {"RUN_ID", r.RUN_ID},
        {"@timestamp", r.timestamp},
        {"durationSeconds", r.durationSeconds},
        {"config", reportConfigJson(r.config)},
        {"results", r.results},
    };
}

void to_json(nlohmann::json& j, const TradingFailure& f) {
    j = nlohmann::json{
        {"RUN_ID", f.RUN_ID},
        {"@timestamp", f.timestamp},
        {"durationSeconds", f.durationSeconds},
        {"reason", f.reason},
        {"config", reportConfigJson(f.config)},
        {"results", f.results},
    };
}
