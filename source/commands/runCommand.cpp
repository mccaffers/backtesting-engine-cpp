// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "runCommand.hpp"

#include <print>
#include <string>

#include "backtestRunner.hpp"
#include "env.hpp"
#include "jsonParser.hpp"
#include "redisRunner.hpp"

int RunCommand::run(int argc, const char* argv[]) {
    if (argc < 3) {
        // C++23 std::println is faster, safer, and cleaner than iostreams
        std::println(stderr, "Usage: BacktestingEngine run <questdb-host>\n"
                             "       BacktestingEngine run <questdb-host> <base64-config>");
        return 1;
    }

    if (argc == 3) {
        return RedisRunner::run(argv[2], env::getOr("REDIS_HOST", "127.0.0.1"));
    }

    // Else we'll just parse one base64 blob
    auto config = JsonParser::parseConfigurationFromBase64(argv[3]);
    return runBacktest(argv[2], config);
}