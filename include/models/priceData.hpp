// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <chrono>
#include <string>

struct PriceData {
    double ask;
    double bid;
    std::chrono::system_clock::time_point timestamp;
    std::string symbol;

    // Constructor for easy creation
    PriceData(double ask, double bid, const std::chrono::system_clock::time_point& ts, const std::string& symbol)
        : ask(ask), bid(bid), timestamp(ts), symbol(symbol) {}

    PriceData() : ask(0.0), bid(0.0), timestamp{}, symbol("") {}
};
