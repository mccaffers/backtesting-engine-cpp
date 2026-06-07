// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

// Backs the `run` subcommand: drains BACKTESTING_QUEUE_RUN (loading each run's
// QuestDB ticks once, then running its strategies), or runs a single Base64
// Configuration supplied directly on the command line.
class RunCommand {
public:
    static int run(int argc, const char* argv[]);
};
