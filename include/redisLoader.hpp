// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>
#include <vector>

// LPUSH pairs with RedisRunner's RPOP so consumers observe FIFO ordering.
class RedisLoader {
public:
    // LPUSHes a single Base64-encoded payload onto queueKey without assuming a
    // payload type (run descriptor or strategy).
    static int loadPayload(const std::string& redisHost,
                           int redisPort,
                           const std::string& queueKey,
                           const std::string& rawJson);

    // LPUSHes many payloads onto queueKey over one connection. Each payload is
    // Base64-encoded; order is preserved (RedisRunner's RPOP then yields them
    // in insertion order).
    static int loadPayloadBatch(const std::string& redisHost,
                                int redisPort,
                                const std::string& queueKey,
                                const std::vector<std::string>& rawJsonPayloads);
};
