// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "loadCommand.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "redisLoader.hpp"

int LoadCommand::run(int argc, const char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " load <path> [path...]"
                  << std::endl;
        return 1;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string path = argv[i];
        std::ifstream ifs(path);
        if (!ifs) {
            std::cerr << "BacktestingEngine: failed to open file: " << path
                      << std::endl;
            return 1;
        }
        std::ostringstream buffer;
        buffer << ifs.rdbuf();
        if (ifs.bad()) {
            std::cerr << "BacktestingEngine: failed to read file: " << path
                      << std::endl;
            return 1;
        }
        const int rc = RedisLoader::load(buffer.str(), "127.0.0.1", 6379,
                                         "strategy_queue");
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}
g