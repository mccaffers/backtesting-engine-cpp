// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Wire shape for an engine-level exception (as opposed to a trading result or
// failure). Captures the failure so it lands in Elasticsearch alongside run
// outcomes for searchability. `source` names the component that caught it
// (e.g. "RedisRunner", "drainRuns", "Operations") and `RUN_ID` is filled when
// a run is in scope, empty for top-level failures.
struct EngineException {
    std::string timestamp;  // serialised as @timestamp for Kibana
    std::string source;
    std::string message;
    std::string RUN_ID;
};

inline void to_json(nlohmann::json& j, const EngineException& e) {
    j = nlohmann::json{
        {"@timestamp", e.timestamp},
        {"source", e.source},
        {"message", e.message},
        {"RUN_ID", e.RUN_ID},
    };
}
