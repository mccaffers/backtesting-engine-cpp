// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <chrono>

enum class Direction {
    LONG,
    SHORT
};

struct Trade {
    std::string id;
    double entryPrice;
    double size;
    std::chrono::system_clock::time_point openTime;
    Direction direction;

    std::string dealReference;
    std::string symbol;
    double scalingFactor;
    double stopDistancePips;
    double limitDistancePips;
    std::string strategyId;
    std::string strategyName;

    double closePrice;
    std::chrono::system_clock::time_point closeTime;
    
    // Default constructor
    Trade() : entryPrice(0), size(0), direction(Direction::LONG),
              scalingFactor(0), stopDistancePips(0), limitDistancePips(0),
              closePrice(0),
              openTime(std::chrono::system_clock::now()) {}
    
    // Copy constructor
    Trade(const Trade& other) = default;
    
    Trade(double price, double quantity, Direction dir) 
        : entryPrice(price), 
          size(quantity), 
          direction(dir),
          scalingFactor(0), stopDistancePips(0), limitDistancePips(0),
          openTime(std::chrono::system_clock::now()) {
        
    }

};
