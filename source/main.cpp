// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

import std;

import loadCommand;
import runCommand;
import ingestCommand;
import liveCommand;
import trackingCommand;
import positionsCommand;
import experimentsCommand;
import analysisCommand;

int main(const int argc, const char* argv[]) {

    if (argc < 2) {
        std::println(std::cerr, "Error: missing subcommand");
        return 1;
    }

    const std::string_view subcommand = argv[1];

    if (subcommand == "load") return LoadCommand::run(argc, argv);
    if (subcommand == "run") return RunCommand::run(argc, argv);
    if (subcommand == "ingest") return IngestCommand::run(argc, argv);
    if (subcommand == "live") return LiveCommand::run(argc, argv);
    if (subcommand == "tracking") return TrackingCommand::run(argc, argv);
    if (subcommand == "positions") return PositionsCommand::run(argc, argv);
    if (subcommand == "experiments") return ExperimentsCommand::run(argc, argv);
    if (subcommand == "analysis") return AnalysisCommand::run(argc, argv);

    std::println(std::cerr, "Error: unknown subcommand '{}'.", subcommand);
    return 1;
}