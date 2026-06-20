// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

import std;  // replaces <iostream>, <string_view>

// backtesting engine headers
import loadCommand;
import runCommand;

// Entry point
int main(const int argc, const char* argv[]) {

  if (argc < 2) {
    std::cerr << "BacktestingEngine: missing a subcommand. See README.md for usage"
              << std::endl;
    return 1;
  }

  const std::string_view subcommand = argv[1];

  // Two paths, load or run
  if (subcommand == "load") return LoadCommand::run();
  if (subcommand == "run")  return RunCommand::run(argc, argv);

  std::cerr << "BacktestingEngine: unknown subcommand. See README.md for usage."
            << std::endl;
            
  return 1;
}
