// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <array>
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

// Every run queue, in the strict priority order workers drain them: a chain
// queue is only reached once every queue before it is empty. Sweeps and
// hand-queued one-offs land on RUN; a run chained by the rolling-window
// ladder lands on the chain queue matching its rung (rung N -> index N, see
// rolling::queueKeyFor). The keys are depth-indexed rather than named after
// the window ("wave 1", not "OFFSET_3") because the descriptor already
// carries LAST_MONTHS/OFFSET_MONTHS — retuning the ladder's windows must
// never rename a queue. Net effect: a fresh grid sweep always preempts the
// chained single-strategy backlog, and LLEN per key reads wave progress.
inline constexpr std::array RUN_QUEUES = std::to_array<const char*>({
    RUN,
    "BACKTESTING_QUEUE_RUN_CHAIN:1",
    "BACKTESTING_QUEUE_RUN_CHAIN:2",
    "BACKTESTING_QUEUE_RUN_CHAIN:3",
});
inline constexpr const char* STRATEGY_PREFIX = "BACKTESTING_QUEUE_STRATEGY:";
inline constexpr const char* STRATEGY_PAYLOAD_PREFIX =
    "BACKTESTING_QUEUE_STRATEGY_PAYLOAD:";

// Safety-net expiry on strategy payload keys so a crashed or abandoned run
// cannot leak them forever; the worker's GETDEL is the normal cleanup. Part
// of the queue contract (shared by the sweep loader and the rolling-window
// re-queue in Operations) so every producer reaps on the same schedule.
inline constexpr long PAYLOAD_TTL_SECONDS = 7L * 24 * 60 * 60;

inline std::string strategyKey(const std::string& runId) {
    return std::string(STRATEGY_PREFIX) + runId;
}

// The RUN_ID segment groups a run's payload keys under one SCAN-able pattern
// (orphan cleanup); the UUID is the strategy's own, minted by the factory.
inline std::string strategyPayloadKey(const std::string& runId,
                                      const std::string& strategyUuid) {
    return std::string(STRATEGY_PAYLOAD_PREFIX) + runId + ":" + strategyUuid;
}

// The experiment queue family: the same run/name-list/payload-key shape as
// the strategy queue above, produced by `experiments <name>` and drained by
// the `analysis` worker. ONE queue, no priority array — RUN_QUEUES exists
// only for the rolling-window ladder, which experiments never join. Payload
// keys reuse PAYLOAD_TTL_SECONDS, so crashed experiment runs are reaped on
// the same schedule as strategy runs.
inline constexpr const char* EXPERIMENT_RUN = "BACKTESTING_QUEUE_EXPERIMENT_RUN";
inline constexpr const char* EXPERIMENT_PREFIX = "BACKTESTING_QUEUE_EXPERIMENT:";
inline constexpr const char* EXPERIMENT_PAYLOAD_PREFIX =
    "BACKTESTING_QUEUE_EXPERIMENT_PAYLOAD:";

inline std::string experimentKey(const std::string& runId) {
    return std::string(EXPERIMENT_PREFIX) + runId;
}

inline std::string experimentPayloadKey(const std::string& runId,
                                        const std::string& experimentUuid) {
    return std::string(EXPERIMENT_PAYLOAD_PREFIX) + runId + ":" + experimentUuid;
}

}  // namespace queue_keys
