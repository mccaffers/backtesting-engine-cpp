// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "ConfigManager.hpp"
#include <iostream>

std::shared_ptr<ConfigManager> ConfigManager::instance = nullptr;
