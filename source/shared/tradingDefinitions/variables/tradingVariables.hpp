// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
struct TradingVariables {
    std::string STRATEGY;
    // SL/TP distances in whole pips; TRADING_SIZE in whole lots. All integer —
    // the engine converts pips->points and applies size inside its integer
    // hot loop (see symbolScale.hpp / tradeManager).
    int32_t STOP_DISTANCE_IN_PIPS = 0;
    int32_t LIMIT_DISTANCE_IN_PIPS = 0;
    int32_t TRADING_SIZE = 0;
};

// Read an integer field that may arrive as a JSON number or, per this codebase's
// string-encoded-numeric convention, as a string. lround matches
// sweep::Combination::getInt and tolerates any legacy decimal value.
inline int32_t readIntField(const nlohmann::json& j) {
    const double value =
        j.is_string() ? std::stod(j.get<std::string>()) : j.get<double>();
    return static_cast<int32_t>(std::lround(value));
}

// Custom (de)serialisation: the fields are int32_t in memory but travel on the
// wire as strings (matching the existing convention), and from_json accepts
// either a string or a number so existing payloads keep parsing.
inline void to_json(nlohmann::json& j, const TradingVariables& v) {
    j = nlohmann::json{
        {"STRATEGY", v.STRATEGY},
        {"STOP_DISTANCE_IN_PIPS", std::to_string(v.STOP_DISTANCE_IN_PIPS)},
        {"LIMIT_DISTANCE_IN_PIPS", std::to_string(v.LIMIT_DISTANCE_IN_PIPS)},
        {"TRADING_SIZE", std::to_string(v.TRADING_SIZE)},
    };
}

inline void from_json(const nlohmann::json& j, TradingVariables& v) {
    j.at("STRATEGY").get_to(v.STRATEGY);
    v.STOP_DISTANCE_IN_PIPS = readIntField(j.at("STOP_DISTANCE_IN_PIPS"));
    v.LIMIT_DISTANCE_IN_PIPS = readIntField(j.at("LIMIT_DISTANCE_IN_PIPS"));
    v.TRADING_SIZE = readIntField(j.at("TRADING_SIZE"));
}
}
