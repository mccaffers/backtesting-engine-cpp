// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

// Redis keys for the two-tier work queue. A sweep produces one run descriptor on
// RUN and N strategy payloads on STRATEGY_PREFIX + RUN_ID, all linked by RUN_ID.
namespace queue_keys {

inline constexpr const char* RUN = "BACKTESTING_QUEUE_RUN";
inline constexpr const char* STRATEGY_PREFIX = "BACKTESTING_QUEUE_STRATEGY:";

inline std::string strategyKey(const std::string& runId) {
    return std::string(STRATEGY_PREFIX) + runId;
}

}  // namespace queue_keys
