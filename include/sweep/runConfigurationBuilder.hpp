// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

#include "run_configuration.hpp"

namespace sweep {

// The run-level descriptor: what tick data to pull from QuestDB, plus the risk
// limits every strategy in the sweep runs under. Shared by every strategy in
// this sweep and linked to them by RUN_ID.
trading_definitions::RunConfiguration makeRunConfiguration(const std::string& runId);

}  // namespace sweep
