// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

// Backs the `run` subcommand: executes one strategy popped from the Redis
// `strategy_queue`, or a Base64 config supplied directly on the command line.
class RunCommand {
public:
    static int run(int argc, const char* argv[]);
};
