// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

// Backs the `load` subcommand: LPUSHes a strategy JSON (defined in
// source/commands/loadCommand.cpp) onto the Redis `strategy_queue`.
class LoadCommand {
public:
    static int run();
};
