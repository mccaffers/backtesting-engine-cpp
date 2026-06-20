// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions.hpp"

class JsonParser {
public:
    static tradingDefinitions::Configuration parseConfigurationFromBase64(const std::string& input);
    static tradingDefinitions::RunConfiguration parseRunConfigurationFromBase64(const std::string& input);
    static tradingDefinitions::Strategy parseStrategyFromBase64(const std::string& input);
};
