// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>

// Worker entry point. Drains BACKTESTING_QUEUE_RUN: for each run it loads the
// QuestDB tick data once, then drains that run's per-RUN_ID strategy list,
// running every strategy against the cached ticks. Safe to launch many workers
// concurrently — they peek the same run, load ticks once each, and compete on
// RPOP of the shared strategy list.
class RedisRunner {
public:
    static int run(const std::string& questdbHost,
                   const std::string& redisHost = "127.0.0.1",
                   int redisPort = 6379);
};
