// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

import std;

// backtesting engine headers
import loadCommand;
import runCommand;

int main(const int argc, const char* argv[]) {

    if (argc < 2) {
        std::println(std::cerr, "Error: missing subcommand");
        return 1;
    }

    const std::string_view subcommand = argv[1];

    if (subcommand == "load") return LoadCommand::run();
    if (subcommand == "run") return RunCommand::run(argc, argv);

    std::println(std::cerr, "Error: unknown subcommand '{}'.", subcommand);
    return 1;
}