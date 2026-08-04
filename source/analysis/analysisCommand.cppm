// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/utilities/env.hpp"
#include "analysis/queue/analysisRunner.hpp"

export module analysisCommand;

import std;

export class AnalysisCommand {
public:
    static int run(int argc, const char* argv[]);
};

int AnalysisCommand::run(const int argc, const char* argv[]) {

    env::printDiagnostics(argc, argv);

    if (argc < 3) {
        std::println(stderr, "Usage: BacktestingEngine analysis <questdb-host>");
        return 1;
    }

    // Redis host from the environment, mirroring `run` (runCommand.cppm).
    return AnalysisRunner::run(argv[2], env::getOr("REDIS_HOST", "127.0.0.1"));
}
