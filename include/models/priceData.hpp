// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <chrono>

struct PriceData {
    double value1;
    double value2;
    std::chrono::system_clock::time_point timestamp;

    // Constructor for easy creation
    PriceData(double v1, double v2, const std::chrono::system_clock::time_point& ts)
        : value1(v1), value2(v2), timestamp(ts) {}

    PriceData() : value1(0.0), value2(0.0), timestamp{} {}
};
