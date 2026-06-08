// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "jsonParser.hpp"
#include "base64.hpp"
#include <iostream>

using json = nlohmann::json;

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
    std::cout << config.RUN_ID << std::endl;

    return config;
}

trading_definitions::RunConfiguration JsonParser::parseRunConfigurationFromBase64(const std::string& input) {
    const std::string output = Base64::b64decode(input);

    json j;
    try {
        j = json::parse(output);
    }
    catch (json::parse_error& ex) {
        std::cerr << "parse error at byte " << ex.byte << std::endl;
    }

    return j.get<trading_definitions::RunConfiguration>();
}

trading_definitions::Strategy JsonParser::parseStrategyFromBase64(const std::string& input) {
    const std::string output = Base64::b64decode(input);

    json j;
    try {
        j = json::parse(output);
    }
    catch (json::parse_error& ex) {
        std::cerr << "parse error at byte " << ex.byte << std::endl;
    }

    return j.get<trading_definitions::Strategy>();
}
