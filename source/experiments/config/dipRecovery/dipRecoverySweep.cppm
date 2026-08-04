// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/experiments/experimentConfig.hpp"

export module dipRecoverySweep;

export import parameterGenerator;  // sweep::ParameterGenerator, Combination

import std;
import makeDipRecovery;  // sweep::makeDipRecoveryExperiment

export namespace sweep {

// Maps a swept combination onto a concrete experiment's config (minting a
// fresh UUID) — the experiment analogue of StrategyFactory.
using ExperimentFactory =
    experiments::ExperimentConfig (*)(const Combination&);

// One experiment sweep, fully specified: the parameter grid, the factory
// that maps each combination onto an ExperimentConfig, and the tick-history
// window the sweep runs over. The window is PER-SWEEP by design — each sweep
// builder declares its own LAST_MONTHS/OFFSET_MONTHS (default 9/0) colocated
// with the grid, rather than a global constant.
struct ExperimentSweepSpec {
    ParameterGenerator generator;
    ExperimentFactory factory = nullptr;
    int lastMonths = 9;
    int offsetMonths = 0;
};

// The first experiment sweep: how often does a sharp dip recover? The grid
// crosses drop size x drop window x recovery size x recovery window.
ExperimentSweepSpec buildDipRecoverySweep() {
    ParameterGenerator generator;
    // How deep the dip is, in percent of the pre-dip price (mapped to a
    // negative DirectionalMove by makeDipRecoveryExperiment).
    generator.addList("LEG1_DROP_PERCENT", {0.25, 0.5, 1.0, 2.0});
    // How fast the dip must land: the trailing window the drop is measured
    // over (rolling extremes, not an anchored start).
    generator.addList("LEG1_WINDOW_MINUTES", {5, 10, 30, 60});
    // How much of the dip must come back...
    generator.addList("LEG2_RISE_PERCENT", {0.1, 0.25, 0.5, 1.0});
    // ...and how quickly, anchored at the dip's completion tick.
    generator.addList("LEG2_WINDOW_MINUTES", {5, 10, 30, 60});
    return ExperimentSweepSpec{
        .generator = std::move(generator),
        .factory = makeDipRecoveryExperiment,
        .lastMonths = 9,
        .offsetMonths = 0,
    };
}

}  // namespace sweep
