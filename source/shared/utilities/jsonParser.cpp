// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/utilities/jsonParser.hpp"
#include "shared/utilities/base64.hpp"

using json = nlohmann::json;

// Parse errors propagate to the caller — json::parse_error carries the byte
// offset, which is more useful than the downstream type_error a discarded
// json value would produce.

tradingDefinitions::Configuration JsonParser::parseConfigurationFromBase64(const std::string& input) {
    return json::parse(Base64::b64decode(input)).get<tradingDefinitions::Configuration>();
}

tradingDefinitions::RunConfiguration JsonParser::parseRunConfigurationFromBase64(const std::string& input) {
    return json::parse(Base64::b64decode(input)).get<tradingDefinitions::RunConfiguration>();
}

tradingDefinitions::Strategy JsonParser::parseStrategyFromBase64(const std::string& input) {
    return json::parse(Base64::b64decode(input)).get<tradingDefinitions::Strategy>();
}
