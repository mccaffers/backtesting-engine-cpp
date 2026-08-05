// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace tradingDefinitions {
struct TradingVariables {
    std::string STRATEGY;
    // SL/TP distances as whole ATR multipliers (distance = ATR x multiplier,
    // computed per entry by conditions::check, which also clamps the result
    // to pip bounds); TRADING_SIZE in whole lots. All integer — the engine
    // converts pips->points and applies size inside its integer hot loop
    // (see symbolScale.hpp / tradeManager).
    int32_t STOP_DISTANCE_IN_ATR = 0;
    int32_t LIMIT_DISTANCE_IN_ATR = 0;
    int32_t TRADING_SIZE = 0;
};

// Read an integer field that may arrive as a JSON number or, per this codebase's
// string-encoded-numeric convention, as a string. lround matches
// sweep::Combination::getInt and tolerates any legacy decimal value.
// Non-finite values ("nan"/"inf" — UB in lround) and values outside int32 are
// rejected: the old unchecked cast silently wrapped e.g. "99999999999" into a
// garbage (possibly negative) pip distance or size, producing wrong trades
// instead of a rejected config.
inline int32_t readIntField(const nlohmann::json& j) {
    const double value =
        j.is_string() ? std::stod(j.get<std::string>()) : j.get<double>();
    if (!std::isfinite(value) ||
        value < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        value > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        throw std::invalid_argument(
            "TradingVariables: integer field out of range: " + j.dump());
    }
    return static_cast<int32_t>(std::lround(value));
}

// Custom (de)serialisation: the fields are int32_t in memory but travel on the
// wire as strings (matching the existing convention), and from_json accepts
// either a string or a number so existing payloads keep parsing.
inline void to_json(nlohmann::json& j, const TradingVariables& v) {
    j = nlohmann::json{
        {"STRATEGY", v.STRATEGY},
        {"STOP_DISTANCE_IN_ATR", std::to_string(v.STOP_DISTANCE_IN_ATR)},
        {"LIMIT_DISTANCE_IN_ATR", std::to_string(v.LIMIT_DISTANCE_IN_ATR)},
        {"TRADING_SIZE", std::to_string(v.TRADING_SIZE)},
    };
}

inline void from_json(const nlohmann::json& j, TradingVariables& v) {
    j.at("STRATEGY").get_to(v.STRATEGY);
    // Hard rename from *_IN_PIPS (no legacy-key fallback, deliberately): the
    // semantics changed from pips to ATR multipliers, so an old payload
    // parsing silently would trade a 100-pip value as a 100x multiplier.
    // Pre-rename Redis payloads and Elasticsearch winners fail loudly here.
    v.STOP_DISTANCE_IN_ATR = readIntField(j.at("STOP_DISTANCE_IN_ATR"));
    v.LIMIT_DISTANCE_IN_ATR = readIntField(j.at("LIMIT_DISTANCE_IN_ATR"));
    v.TRADING_SIZE = readIntField(j.at("TRADING_SIZE"));
}
}
