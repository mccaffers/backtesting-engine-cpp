// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "runCommand.hpp"

#include <iostream>
#include <string>

#include "backtestRunner.hpp"
#include "jsonParser.hpp"
#include "redisRunner.hpp"

namespace {

int runBacktestFromBase64(const std::string& questdbHost,
                          const std::string& base64Config) {
    auto config = JsonParser::parseConfigurationFromBase64(base64Config);
    return runBacktest(questdbHost, config);
}

}  // namespace

int RunCommand::run(int argc, const char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: BacktestingEngine run <questdb-host>\n"
                  << "       BacktestingEngine run <questdb-host> <base64-config>"
                  << std::endl;
        return 1;
    }
    if (argc == 3) {
        return RedisRunner::run(argv[2]);
    }
    return runBacktestFromBase64(argv[2], argv[3]);
}
