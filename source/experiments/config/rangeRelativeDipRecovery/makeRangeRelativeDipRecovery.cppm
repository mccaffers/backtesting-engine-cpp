// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/experiments/experimentConfig.hpp"

export module makeRangeRelativeDipRecovery;

import std;
import sweepCombination;  // sweep::Combination

export namespace sweep {

// Map one swept parameter combination onto a rangeRelativeDipRecovery experiment:
// "Leg 1: RangeRelativeMove dip (DIRECTION = -1) with LEG1_LOOKBACK_MINUTES,
//  LEG1_WINDOW_MINUTES, and LEG1_ATR_MULTIPLE.
//  Leg 2: RangeRelativeMove recovery (DIRECTION = 1) with LEG1_LOOKBACK_MINUTES,
//  LEG2_WINDOW_MINUTES, and LEG2_ATR_MULTIPLE".
experiments::ExperimentConfig makeRangeRelativeDipRecoveryExperiment(
    const sweep::Combination& combo) {
    using experiments::Activity;
    using experiments::ActivityType;
    return experiments::ExperimentConfig{
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .NAME = "rangeRelativeDipRecovery",
        .CHAIN = {
            // Leg 1: the dip — RangeRelativeMove downwards (DIRECTION = -1)
            Activity{
                .TYPE = ActivityType::RangeRelativeMove,
                .WINDOW_SECONDS = combo.getInt("LEG1_WINDOW_MINUTES") * 60,
                .LOOKBACK_SECONDS = combo.getInt("LEG1_LOOKBACK_MINUTES") * 60,
                .DIRECTION = -1,
                .ATR_MULTIPLE = combo.get("LEG1_ATR_MULTIPLE"),
            },
            // Leg 2: the recovery — RangeRelativeMove upwards (DIRECTION = 1)
            // Basis lookback matches leg 1's lookback basis
            Activity{
                .TYPE = ActivityType::RangeRelativeMove,
                .WINDOW_SECONDS = combo.getInt("LEG2_WINDOW_MINUTES") * 60,
                .LOOKBACK_SECONDS = combo.getInt("LEG1_LOOKBACK_MINUTES") * 60,
                .DIRECTION = 1,
                .ATR_MULTIPLE = combo.get("LEG2_ATR_MULTIPLE"),
            },
        },
    };
}

}  // namespace sweep
