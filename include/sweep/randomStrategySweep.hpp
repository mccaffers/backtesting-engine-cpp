// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include "parameterSweep.hpp"

namespace sweep {

// Declares which parameters to sweep for the RandomStrategy. Keeping the ranges
// in one place means a new strategy (or extra swept parameter) is a localised
// edit: register it here, then read it back in makeStrategy().
ParameterGenerator buildRandomStrategySweep();

}  // namespace sweep
