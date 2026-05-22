// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// std headers
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// backtesting engine headers
#include "backtestRunner.hpp"
#include "jsonParser.hpp"
#include "redisLoader.hpp"
#include "redisRunner.hpp"

static int runBacktest(const std::string& questdbHost,
                       const std::string& base64Config) {
  auto config = JsonParser::parseConfigurationFromBase64(base64Config);
  return runBacktest(questdbHost, config);
}

static void printUsage(std::ostream& out) {
  out << "Usage: BacktestingEngine <subcommand> [args...]\n"
      << "\n"
      << "Subcommands:\n"
      << "  load <path> [path...]         Push Base64-encoded JSON strategies\n"
      << "                                from path(s) onto Redis\n"
      << "                                `strategy_queue`.\n"
      << "  run <questdb-host>            Pop one Base64 strategy from the\n"
      << "                                Redis `strategy_queue` and execute\n"
      << "                                it.\n"
      << "  run <questdb-host> <base64-config>\n"
      << "                                Decode the supplied Base64 strategy\n"
      << "                                and execute it.\n"
      << "  -h, --help                    Show this help message."
      << std::endl;
}

// Entry point. Dispatches on argv[1] to one of the BacktestingEngine
// subcommands: `load <raw-json>` is the Redis enqueue path — it LPUSHes a
// base64-encoded JSON payload onto `strategy_queue` via RedisLoader, pairing
// with the dequeue side handled by `RedisRunner`; `run <questdb-host>` RPOPs
// the next strategy from `strategy_queue` and executes it against QuestDB,
// while `run <questdb-host> <base64-config>` skips Redis and executes the
// supplied Base64 strategy directly; `-h`/`--help` prints usage.
int main(int argc, const char * argv[]) {
  if (argc < 2) {
    printUsage(std::cerr);
    return 1;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "-h" || subcommand == "--help") {
    printUsage(std::cout);
    return 0;
  }

  if (subcommand == "load") {
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

  if (subcommand == "run") {
    if (argc < 3) {
      std::cerr << "Usage: BacktestingEngine run <questdb-host>\n"
                << "       BacktestingEngine run <questdb-host> <base64-config>"
                << std::endl;
      return 1;
    }
    if (argc == 3) {
      return RedisRunner::run(argv[2]);
    }
    return runBacktest(argv[2], argv[3]);
  }

  printUsage(std::cerr);
  return 1;
}
