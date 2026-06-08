// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "trading_definitions.hpp"

class JsonParser {
public:
    static trading_definitions::Configuration parseConfigurationFromBase64(const std::string& input);
    static trading_definitions::RunConfiguration parseRunConfigurationFromBase64(const std::string& input);
    static trading_definitions::Strategy parseStrategyFromBase64(const std::string& input);
};
