// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace tradingDefinitions {
// One (bar count, bar minutes) pair for a strategy's OHLC feed. {0, 0} is the
// "unused" sentinel carried by strategies that build no bars (RandomStrategy);
// a strategy that actually builds bars must have both >= 1 — calculateOHLC
// throws on a non-positive duration, since a zero-minute bar would otherwise
// roll a new bar on every tick and grow without bound.
struct OHLCVariables {
    int OHLC_COUNT = 0;
    int OHLC_MINUTES = 0;
};

inline void to_json(nlohmann::json& j, const OHLCVariables& v) {
    j = nlohmann::json{
        {"OHLC_COUNT", v.OHLC_COUNT},
        {"OHLC_MINUTES", v.OHLC_MINUTES},
    };
}

inline void from_json(const nlohmann::json& j, OHLCVariables& v) {
    j.at("OHLC_COUNT").get_to(v.OHLC_COUNT);
    j.at("OHLC_MINUTES").get_to(v.OHLC_MINUTES);
    // Zero is the documented "unused" sentinel; anything negative is garbage
    // that should fail the parse (a poison-pill payload) rather than reach the
    // bar builder.
    if (v.OHLC_COUNT < 0 || v.OHLC_MINUTES < 0) {
        throw std::invalid_argument(
            "OHLCVariables: OHLC_COUNT/OHLC_MINUTES must be >= 0, got " + j.dump());
    }
}
}
