// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <chrono>
#include <string>
#include <boost/decimal.hpp>

// decimal64_t: 16 significant base-10 digits, no binary floating-point drift on
// values like 1.23456. Closest C# analogue is System.Decimal (though decimal64_t
// is 64-bit IEEE 754-2008 vs C#'s 128-bit type).
struct PriceData {
    boost::decimal::decimal64_t ask;
    boost::decimal::decimal64_t bid;
    std::chrono::system_clock::time_point timestamp;
    std::string symbol;

    PriceData(boost::decimal::decimal64_t ask, boost::decimal::decimal64_t bid,
              const std::chrono::system_clock::time_point& ts, const std::string& symbol)
        : ask(ask), bid(bid), timestamp(ts), symbol(symbol) {}

    PriceData() : ask(0), bid(0), timestamp{}, symbol("") {}
};
