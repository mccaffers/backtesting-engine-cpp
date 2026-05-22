// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include "trading_definitions/configuration.hpp"

int runBacktest(const std::string& questdbHost,
                const trading_definitions::Configuration& config);
