// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/experiments/experimentConfig.hpp"

export module rangeRelativeDipRecoverySweep;

export import parameterGenerator;  // sweep::ParameterGenerator, Combination

import std;
import dipRecoverySweep;  // sweep::ExperimentSweepSpec, ExperimentFactory
import makeRangeRelativeDipRecovery;  // sweep::makeRangeRelativeDipRecoveryExperiment

export namespace sweep {

// Experiment sweep using ActivityType::RangeRelativeMove:
// How often does a volatility-scaled dip recover?
// Grid:
//   LEG1_LOOKBACK_MINUTES {15, 30, 60}
//   LEG1_ATR_MULTIPLE {1.0, 1.5, 2.0, 2.5}
//   LEG1_WINDOW_MINUTES {5, 10, 30}
//   LEG2_ATR_MULTIPLE {0.25, 0.5, 1.0}
//   LEG2_WINDOW_MINUTES {5, 10, 30}
ExperimentSweepSpec buildRangeRelativeDipRecoverySweep() {
    ParameterGenerator generator;
    generator.addList("LEG1_LOOKBACK_MINUTES", {15, 30, 60});
    generator.addList("LEG1_ATR_MULTIPLE", {1.0, 1.5, 2.0, 2.5});
    generator.addList("LEG1_WINDOW_MINUTES", {5, 10, 30});
    generator.addList("LEG2_ATR_MULTIPLE", {0.25, 0.5, 1.0});
    generator.addList("LEG2_WINDOW_MINUTES", {5, 10, 30});
    return ExperimentSweepSpec{
        .generator = std::move(generator),
        .factory = makeRangeRelativeDipRecoveryExperiment,
        .lastMonths = 9,
        .offsetMonths = 0,
    };
}

}  // namespace sweep
