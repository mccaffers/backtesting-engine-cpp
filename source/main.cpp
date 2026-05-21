// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// std headers
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Single TU that pulls in the Boost.Redis implementation.
#include <boost/redis/src.hpp>

// backtesting engine headers
#include "redisLoader.hpp"
#include "redisRunner.hpp"

static void printUsage(std::ostream& out) {
  out << "Usage: BacktestingEngine <subcommand> [args...]\n"
      << "\n"
      << "Subcommands:\n"
      << "  load <strategy.json|dir>...   LPUSH base64-encoded strategy JSON(s)\n"
      << "                                onto the Redis `strategy_queue`.\n"
      << "  run <questdb-host>            RPOP a strategy from `strategy_queue`\n"
      << "                                and execute it against QuestDB.\n"
      << "  -h, --help                    Show this help message."
      << std::endl;
}

static int loadStrategies(const std::vector<std::string>& paths) {
  namespace fs = std::filesystem;

  std::vector<fs::path> jsonFiles;
  for (const auto& raw : paths) {
    fs::path p(raw);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      for (auto it = fs::recursive_directory_iterator(p, ec);
           !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".json") {
          jsonFiles.push_back(it->path());
        }
      }
      if (ec) {
        std::cerr << "load: failed to enumerate " << p << ": " << ec.message()
                  << std::endl;
        return 1;
      }
    } else if (fs::is_regular_file(p, ec)) {
      jsonFiles.push_back(p);
    } else {
      std::cerr << "load: path is not a file or directory: " << p << std::endl;
      return 1;
    }
  }

  if (jsonFiles.empty()) {
    std::cerr << "load: no strategy JSON files found" << std::endl;
    return 1;
  }

  for (const auto& file : jsonFiles) {
    std::ifstream in(file);
    if (!in) {
      std::cerr << "load: failed to open " << file << std::endl;
      return 1;
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    const int rc = RedisLoader::load(buf.str());
    if (rc != 0) {
      return rc;
    }
  }

  return 0;
}

// Entry point. Dispatches on argv[1] to one of the BacktestingEngine
// subcommands: `load <strategy.json|dir>...` enqueues base64-encoded
// strategy JSON onto the Redis `strategy_queue` (LPUSH); `run <questdb-host>`
// RPOPs the next strategy and executes it against QuestDB; `-h`/`--help`
// prints usage.
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
    std::vector<std::string> paths(argv + 2, argv + argc);
    if (paths.empty()) {
      printUsage(std::cerr);
      return 1;
    }
    try {
      return loadStrategies(paths);
    } catch (const std::exception& ex) {
      std::cerr << "load: " << ex.what() << std::endl;
      return 1;
    }
  }

  if (subcommand == "run") {
    if (argc < 3) {
      std::cerr << "Usage: BacktestingEngine run <questdb-host>" << std::endl;
      return 1;
    }
    return RedisRunner::run(argv[2]);
  }

  printUsage(std::cerr);
  return 1;
}
