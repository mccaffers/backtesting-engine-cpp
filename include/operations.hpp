// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <vector>
#include "models/priceData.hpp"

class Operations {
    
public:
    static void run(const std::vector<PriceData>& priceData);
};
