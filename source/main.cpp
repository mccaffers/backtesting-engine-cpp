// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// std headers
#include <iostream>
#include <string_view>

// backtesting engine headers
#include "loadCommand.hpp"
#include "runCommand.hpp"

int main(int argc, const char* argv[]) {
  if (argc < 2) {
    std::cerr << "BacktestingEngine: missing subcommand. See README.md for usage."
              << std::endl;
    return 1;
  }

  const std::string_view subcommand = argv[1];

  if (subcommand == "load") return LoadCommand::run(argc, argv);
  if (subcommand == "run")  return RunCommand::run(argc, argv);

  std::cerr << "BacktestingEngine: unknown subcommand. See README.md for usage."
            << std::endl;
  return 1;
}
