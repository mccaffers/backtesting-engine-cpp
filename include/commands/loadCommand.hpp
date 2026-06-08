// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

// Backs the `load` subcommand: for one RUN_ID, LPUSHes every swept strategy onto
// BACKTESTING_QUEUE_STRATEGY:<RUN_ID>, then LPUSHes the run descriptor onto
// BACKTESTING_QUEUE_RUN (see source/commands/loadCommand.cpp).
class LoadCommand {
public:
    static int run();
};
