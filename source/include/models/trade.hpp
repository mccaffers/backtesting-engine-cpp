// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <chrono>

struct Trade {
    static int idCounter;
    std::string id;
    double entryPrice;
    double size;
    std::chrono::system_clock::time_point openTime;
    bool isLong;
    
    // Default constructor
    Trade() : entryPrice(0), size(0), isLong(false), 
              openTime(std::chrono::system_clock::now()) {}
    
    // Copy constructor
    Trade(const Trade& other) = default;
    
    Trade(double price, double quantity, bool long_position) 
        : entryPrice(price), 
          size(quantity), 
          isLong(long_position),
          openTime(std::chrono::system_clock::now()) {
        // Generate unique ID using counter
        id = std::to_string(++idCounter);
    }

    static void resetCounter() {
        idCounter = 0;
    }
};
