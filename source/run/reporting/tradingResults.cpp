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

void to_json(nlohmann::json& j, const TradingResultsStats& s) {
    j = nlohmann::json{
        {"finalPnl", s.finalPnl},
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
        {"performanceScore", s.performanceScore},
        {"winRate", s.winRate},
        {"tradeRatio", s.tradeRatio},
        {"expectancyScore", s.expectancyScore},
        {"calmarScore", s.calmarScore},
        {"confidenceMultiplier", s.confidenceMultiplier},
        {"maxDrawdownPercent", s.maxDrawdownPercent},
    };
    if (s.avgPnl) {
        j["avgPnl"] = *s.avgPnl;
    } else {
        j["avgPnl"] = nullptr;
    }
}

void to_json(nlohmann::json& j, const TradingResults& r) {
    j = nlohmann::json{
        {"RUN_ID", r.RUN_ID},
        {"@timestamp", r.timestamp},
        {"durationSeconds", r.durationSeconds},
        {"config", r.config},
        {"results", r.results},
    };
}

void to_json(nlohmann::json& j, const TradingFailure& f) {
    j = nlohmann::json{
        {"RUN_ID", f.RUN_ID},
        {"@timestamp", f.timestamp},
        {"durationSeconds", f.durationSeconds},
        {"reason", f.reason},
        {"config", f.config},
        {"results", f.results},
    };
}
