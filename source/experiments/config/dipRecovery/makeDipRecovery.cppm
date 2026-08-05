// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/experiments/experimentConfig.hpp"

export module makeDipRecovery;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a dipRecovery experiment: "price
// drops LEG1_DROP_PERCENT within LEG1_WINDOW_MINUTES, then rises
// LEG2_RISE_PERCENT within a further LEG2_WINDOW_MINUTES". Every parameter is
// read with get/getInt and no has() fallback: the sweep registers every name
// read here (buildDipRecoverySweep), so a missing one is a bug that should
// throw at queue time — the makeFvgStrategy doctrine.
experiments::ExperimentConfig makeDipRecoveryExperiment(
    const sweep::Combination& combo) {
    using experiments::Activity;
    using experiments::ActivityType;
    return experiments::ExperimentConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .NAME = "dipRecovery",
        .CHAIN = {
            // Leg 1: the dip — a NEGATIVE directional move (the grid sweeps
            // the drop as a positive magnitude; the sign lives here).
            Activity{
                .TYPE = ActivityType::DirectionalMove,
                .MOVE_PERCENT = -combo.get("LEG1_DROP_PERCENT"),
                .WINDOW_SECONDS = combo.getInt("LEG1_WINDOW_MINUTES") * 60,
            },
            // Leg 2: the recovery — a positive move from the dip's
            // completion tick (anchored by the matcher).
            Activity{
                .TYPE = ActivityType::DirectionalMove,
                .MOVE_PERCENT = combo.get("LEG2_RISE_PERCENT"),
                .WINDOW_SECONDS = combo.getInt("LEG2_WINDOW_MINUTES") * 60,
            },
        },
    };
}

}  // namespace sweep
