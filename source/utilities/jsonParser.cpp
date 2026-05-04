// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "jsonParser.hpp"
#include "base64.hpp"
#include "symbolScale.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// Splits "EURUSD,USDJPY" into {"EURUSD","USDJPY"}, ignoring empty tokens.
std::vector<std::string> splitSymbols(const std::string& csv) {
    std::vector<std::string> out;
    std::istringstream ss(csv);
    for (std::string token; std::getline(ss, token, ',');) {
        if (!token.empty()) out.push_back(token);
    }
    return out;
}

void validate(const trading_definitions::Configuration& c) {
    if (c.RUN_ID.empty()) {
        throw std::invalid_argument("Configuration: RUN_ID must not be empty");
    }
    if (c.SYMBOLS.empty()) {
        throw std::invalid_argument("Configuration: SYMBOLS must not be empty");
    }
    const auto symbols = splitSymbols(c.SYMBOLS);
    if (symbols.empty()) {
        throw std::invalid_argument("Configuration: SYMBOLS contains no valid entries");
    }
    for (const auto& sym : symbols) {
        if (symbol_scale::get(sym) == symbol_scale::kUnknown) {
            throw std::invalid_argument("Configuration: unknown symbol '" + sym + "' (no pip scale defined in symbol_scale::kTable)");
        }
    }
    if (c.LAST_MONTHS <= 0) {
        throw std::invalid_argument("Configuration: LAST_MONTHS must be positive (got " + std::to_string(c.LAST_MONTHS) + ")");
    }

    const auto& s = c.STRATEGY;
    if (s.UUID.empty()) {
        throw std::invalid_argument("Configuration: STRATEGY.UUID must not be empty");
    }

    const auto& tv = s.TRADING_VARIABLES;
    if (tv.STRATEGY.empty()) {
        throw std::invalid_argument("Configuration: TRADING_VARIABLES.STRATEGY must not be empty");
    }
    if (!(tv.TRADING_SIZE > 0.0)) {
        throw std::invalid_argument("Configuration: TRADING_VARIABLES.TRADING_SIZE must be positive");
    }
    if (tv.STOP_DISTANCE_IN_PIPS < 0.0) {
        throw std::invalid_argument("Configuration: TRADING_VARIABLES.STOP_DISTANCE_IN_PIPS must be >= 0");
    }
    if (tv.LIMIT_DISTANCE_IN_PIPS < 0.0) {
        throw std::invalid_argument("Configuration: TRADING_VARIABLES.LIMIT_DISTANCE_IN_PIPS must be >= 0");
    }

    for (size_t i = 0; i < s.OHLC_VARIABLES.size(); ++i) {
        const auto& o = s.OHLC_VARIABLES[i];
        if (o.OHLC_COUNT <= 0) {
            throw std::invalid_argument("Configuration: OHLC_VARIABLES[" + std::to_string(i) + "].OHLC_COUNT must be positive");
        }
        if (o.OHLC_MINUTES <= 0) {
            throw std::invalid_argument("Configuration: OHLC_VARIABLES[" + std::to_string(i) + "].OHLC_MINUTES must be positive");
        }
    }

    if (s.STRATEGY_VARIABLES.OHLC_RSI_VARIABLES.has_value()) {
        const auto& rsi = *s.STRATEGY_VARIABLES.OHLC_RSI_VARIABLES;
        // RSI is bounded to [0, 100]; the trigger thresholds must lie strictly inside.
        if (rsi.RSI_LONG  < 1 || rsi.RSI_LONG  > 99 ||
            rsi.RSI_SHORT < 1 || rsi.RSI_SHORT > 99) {
            throw std::invalid_argument("Configuration: OHLC_RSI_VARIABLES thresholds must be in [1, 99]");
        }
    }
}

} // namespace

trading_definitions::Configuration JsonParser::parseConfigurationFromBase64(const std::string& input) {
    // Ingest parameters
    std::string output = Base64::b64decode(input);
    
    // Debug, console print out
    std::cout << output;
    
    json j;
    try {
        j = json::parse(output);
    }
    catch (json::parse_error& ex) {
        std::cerr << "parse error at byte " << ex.byte << std::endl;
    }
    
    auto config = j.get<trading_definitions::Configuration>();
    validate(config);
    std::cout << config.RUN_ID << std::endl;

    return config;
}
