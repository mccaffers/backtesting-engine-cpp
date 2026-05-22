// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>

class RedisRunner {
public:
    static int run(const std::string& questdbHost,
                   const std::string& redisHost = "127.0.0.1",
                   int redisPort = 6379,
                   const std::string& queueKey = "strategy_queue");
};
