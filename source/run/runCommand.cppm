// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/utilities/env.hpp"
#include "shared/utilities/jsonParser.hpp"
#include "run/queue/redisRunner.hpp"

export module runCommand;

import std;
import backtestRunner;

export class RunCommand {
public:
    static int run(int argc, const char* argv[]);
};

int RunCommand::run(const int argc, const char* argv[]) {

    env::printDiagnostics(argc, argv);

    if (argc < 3) {
        std::println(stderr, "Usage: BacktestingEngine run <questdb-host>\n"
                             "       BacktestingEngine run <questdb-host> <base64-config>");
        return 1;
    }

    if (argc == 3) {
        return RedisRunner::run(argv[2], env::getOr("REDIS_HOST", "127.0.0.1"));
    }

    const auto config = JsonParser::parseConfigurationFromBase64(argv[3]);
    return runBacktest(argv[2], config);
}
