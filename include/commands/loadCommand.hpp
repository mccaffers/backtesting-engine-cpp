// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

// Backs the `load` subcommand: reads each strategy file from disk and LPUSHes
// it onto the Redis `strategy_queue` via RedisLoader.
class LoadCommand {
public:
    static int run(int argc, const char* argv[]);
};
