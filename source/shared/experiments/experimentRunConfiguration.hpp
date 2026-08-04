// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Intentionally a plain header, NOT a .cppm module — see runConfiguration.hpp
// for why JSON-serializable structs stay GMF-includable headers in this repo.
namespace experiments {

// Run-level descriptor for an experiment run: the unit of QuestDB tick data
// shared by every experiment in a sweep. Carried on
// BACKTESTING_QUEUE_EXPERIMENT_RUN and linked to its experiment payloads via
// RUN_ID (see queueKeys.hpp) — RunConfiguration's exact pattern, minus the
// trading risk knobs an occurrence count has no use for.
//
// The tick window is LAST_MONTHS long and ends OFFSET_MONTHS before now; each
// sweep builder declares its own pair (default 9/0 — see ExperimentSweepSpec).
// BATCH / EXECUTION_TS are the batch identity minted once per `experiments`
// invocation, naming the weekly backtesting-experiments index the results
// land in (same doctrine as RunConfiguration's fields).
struct ExperimentRunConfiguration {
    std::string RUN_ID;
    std::string SYMBOLS;
    std::string BATCH;
    std::string EXECUTION_TS;
    int LAST_MONTHS = 0;
    int OFFSET_MONTHS = 0;
};

// Hand-written (rather than NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE) so the core
// fields stay strictly required while the rest fall back to the struct
// defaults — RunConfiguration's serializer pattern.
inline void to_json(nlohmann::json& j, const ExperimentRunConfiguration& c) {
    j = nlohmann::json{
        {"RUN_ID", c.RUN_ID},
        {"SYMBOLS", c.SYMBOLS},
        {"BATCH", c.BATCH},
        {"EXECUTION_TS", c.EXECUTION_TS},
        {"LAST_MONTHS", c.LAST_MONTHS},
        {"OFFSET_MONTHS", c.OFFSET_MONTHS},
    };
}

inline void from_json(const nlohmann::json& j, ExperimentRunConfiguration& c) {
    j.at("RUN_ID").get_to(c.RUN_ID);
    j.at("SYMBOLS").get_to(c.SYMBOLS);
    j.at("LAST_MONTHS").get_to(c.LAST_MONTHS);
    const ExperimentRunConfiguration defaults{};
    c.BATCH = j.value("BATCH", defaults.BATCH);
    c.EXECUTION_TS = j.value("EXECUTION_TS", defaults.EXECUTION_TS);
    c.OFFSET_MONTHS = j.value("OFFSET_MONTHS", defaults.OFFSET_MONTHS);
}

}  // namespace experiments
