// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

// LPUSH pairs with RedisRunner's RPOP so consumers observe FIFO ordering.
class RedisLoader {
public:
    static int load(const std::string& rawJson,
                    const std::string& redisHost = "127.0.0.1",
                    int redisPort = 6379,
                    const std::string& queueKey = "strategy_queue");

    static int loadPayload(const std::string& redisHost,
                           int redisPort,
                           const std::string& queueKey,
                           const std::string& rawJson);
};
