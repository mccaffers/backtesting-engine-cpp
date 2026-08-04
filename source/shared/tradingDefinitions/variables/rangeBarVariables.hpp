// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// One range-bar series spec for a strategy's feed. A range bar completes when
// price travels a threshold distance; the threshold is a percentage of the
// rolling high-low range over the last RANGE_ATR_TICK_WINDOW ticks — a pure
// tick-count measure with no clock dependency, so it adapts to a volatility
// spike on the tick it happens rather than when a time bucket rolls (see
// rangeBarBuilder). All-zeros is the "unused" sentinel mirroring
// OHLCVariables' {0, 0}; a strategy that actually builds range bars must have
// every field >= 1 — RangeSeries throws on anything less, since a zero tick
// window can never warm and a zero threshold percent would roll a bar per
// tick.
struct RangeBarVariables {
    int RANGE_ATR_TICK_WINDOW = 0;  // rolling tick window for the range measure
    int RANGE_ATR_PERCENT = 0;      // threshold = windowRange * pct / 100
    int RANGE_COUNT = 0;            // bars kept AND the prepopulate depth
};

inline void to_json(nlohmann::json& j, const RangeBarVariables& v) {
    j = nlohmann::json{
        {"RANGE_ATR_TICK_WINDOW", v.RANGE_ATR_TICK_WINDOW},
        {"RANGE_ATR_PERCENT", v.RANGE_ATR_PERCENT},
        {"RANGE_COUNT", v.RANGE_COUNT},
    };
}

inline void from_json(const nlohmann::json& j, RangeBarVariables& v) {
    j.at("RANGE_ATR_TICK_WINDOW").get_to(v.RANGE_ATR_TICK_WINDOW);
    j.at("RANGE_ATR_PERCENT").get_to(v.RANGE_ATR_PERCENT);
    j.at("RANGE_COUNT").get_to(v.RANGE_COUNT);
    // Zero is the documented "unused" sentinel; anything negative is garbage
    // that should fail the parse (a poison-pill payload) rather than reach the
    // bar builder — same doctrine as OHLCVariables.
    if (v.RANGE_ATR_TICK_WINDOW < 0 || v.RANGE_ATR_PERCENT < 0 ||
        v.RANGE_COUNT < 0) {
        throw std::invalid_argument(
            "RangeBarVariables: all fields must be >= 0, got " + j.dump());
    }
}
}
