// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

// Redis keys for the work queue. A sweep produces, linked by RUN_ID:
//  - one run descriptor on RUN;
//  - one strategy payload per combination, each under its own
//    STRATEGY_PAYLOAD_PREFIX + RUN_ID + ":" + UUID string key (spreading the
//    sweep across many small keys instead of one unbounded list);
//  - the STRATEGY_PREFIX + RUN_ID list, holding just those payload KEY NAMES.
// Workers RPOP a key name (each name goes to exactly one consumer) and GETDEL
// the payload (consumed exactly once, nothing left behind).
namespace queue_keys {

inline constexpr const char* RUN = "BACKTESTING_QUEUE_RUN";
inline constexpr const char* STRATEGY_PREFIX = "BACKTESTING_QUEUE_STRATEGY:";
inline constexpr const char* STRATEGY_PAYLOAD_PREFIX =
    "BACKTESTING_QUEUE_STRATEGY_PAYLOAD:";

inline std::string strategyKey(const std::string& runId) {
    return std::string(STRATEGY_PREFIX) + runId;
}

// The RUN_ID segment groups a run's payload keys under one SCAN-able pattern
// (orphan cleanup); the UUID is the strategy's own, minted by the factory.
inline std::string strategyPayloadKey(const std::string& runId,
                                      const std::string& strategyUuid) {
    return std::string(STRATEGY_PAYLOAD_PREFIX) + runId + ":" + strategyUuid;
}

}  // namespace queue_keys
