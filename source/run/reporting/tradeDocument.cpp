// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/reporting/tradeDocument.hpp"

#include <cstdio>
#include <ctime>  // POSIX gmtime_r

std::string TradeDocument::isoUtcMillis(std::chrono::system_clock::time_point tp) {
    const auto sinceEpoch = tp.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch - secs);
    std::time_t t = secs.count();
    // duration_cast truncates toward zero, so a pre-epoch time point would
    // yield a negative millisecond remainder; fold it into the seconds.
    if (millis.count() < 0) {
        t -= 1;
        millis += std::chrono::milliseconds{1000};
    }
    std::tm utc{};
    gmtime_r(&t, &utc);
    char datePart[32];
    std::strftime(datePart, sizeof(datePart), "%Y-%m-%dT%H:%M:%S", &utc);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s.%03dZ", datePart,
                  static_cast<int>(millis.count()));
    return std::string{buf};
}

void to_json(nlohmann::json& j, const TradeDocument& t) {
    j = nlohmann::json{
        {"RUN_ID", t.RUN_ID},
        {"@timestamp", t.timestamp},
        {"ingestedAt", t.ingestedAt},
        {"hostname", t.hostname},
        {"strategyUuid", t.strategyUuid},
        {"strategyName", t.strategyName},
        {"tradeId", t.tradeId},
        {"symbol", t.symbol},
        {"direction", t.direction},
        {"size", t.size},
        {"entryPrice", t.entryPrice},
        {"entryBid", t.entryBid},
        {"entryAsk", t.entryAsk},
        {"closePrice", t.closePrice},
        {"stopDistancePips", t.stopDistancePips},
        {"limitDistancePips", t.limitDistancePips},
        {"openTime", t.openTime},
        {"closeTime", t.closeTime},
        {"holdSeconds", t.holdSeconds},
        {"pnlPoints", t.pnlPoints},
        {"pnlPips", t.pnlPips},
        {"liquidated", t.liquidated},
        {"scalingFactor", t.scalingFactor},
        {"priceScale", t.priceScale},
    };
    // A zero distance means that exit leg was never armed — the precomputed
    // trigger price is meaningless, so emit null rather than a fake level.
    if (t.stopDistancePips != 0) {
        j["stopPrice"] = t.stopPrice;
    } else {
        j["stopPrice"] = nullptr;
    }
    if (t.limitDistancePips != 0) {
        j["limitPrice"] = t.limitPrice;
    } else {
        j["limitPrice"] = nullptr;
    }
}
