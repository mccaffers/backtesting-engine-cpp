// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/utilities/env.hpp"
#include "shared/utilities/jsonParser.hpp"
#include "shared/redis/redisRunner.hpp"

export module runCommand;

import std;             // replaces <print>, <string>
import backtestRunner;  // runBacktest

// Backs the `run` subcommand: drains BACKTESTING_QUEUE_RUN (loading each run's
// QuestDB ticks once, then running its strategies), or runs a single Base64
// Configuration supplied directly on the command line.
export class RunCommand {
public:
    static int run(int argc, const char* argv[]);
};

int RunCommand::run(int argc, const char* argv[]) {

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
