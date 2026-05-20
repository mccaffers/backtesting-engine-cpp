// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include "tradeManager.hpp"

class Reporting {

public:
    static void summarise(const TradeManager& tradeManager);
};
