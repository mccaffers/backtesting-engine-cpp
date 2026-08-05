// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "shared/experiments/experimentConfig.hpp"
#include "shared/experiments/experimentRunConfiguration.hpp"

// Wire shape pushed to Elasticsearch by the analysis worker: ONE aggregate
// document per experiment x symbol group — the run descriptor and the full
// experiment chain echoed for Kibana filtering, plus the occurrence counts.
// Serialises the timestamp as `@timestamp` for Kibana (TradingResults'
// convention).
struct ExperimentResults {
    std::string RUN_ID;
    std::string timestamp;      // serialised as @timestamp
    std::string hostname;       // machine that ran the analysis
    double durationSeconds = 0.0;  // wall-clock seconds for this evaluation
    experiments::ExperimentRunConfiguration runConfig;  // descriptor echo
    experiments::ExperimentConfig experiment;           // full CHAIN echo
    std::uint64_t occurrences = 0;
    std::uint64_t ticksScanned = 0;
    double daysSpanned = 0.0;
    double occurrencesPerDay = 0.0;  // 0 when the stream spanned no time
    // Conditionality: leg-1 completions (the attempt denominator) and chain
    // deaths attributed to the leg being sought — completionRate is DERIVED
    // in to_json (null, never a fake 0, when attempts == 0).
    std::uint64_t attempts = 0;
    std::vector<std::uint64_t> failuresByLeg;
    // Stability: completions bucketed by the completing tick's UTC calendar
    // slot (absent months are absent keys) and hour-of-day.
    std::map<std::string, std::uint64_t> occurrencesByMonth;
    std::array<std::uint64_t, 24> occurrencesByHourUtc{};
    // Per-symbol breakdown; the flat v1 map stays for compatibility.
    std::map<std::string, experiments::SymbolOutcome> perSymbol;
    std::map<std::string, std::uint64_t> occurrencesBySymbol;
    // Magnitude/timing/cost (phase 2) — see ExperimentOutcome for the
    // semantics; nullopt populations serialise as JSON null.
    std::optional<experiments::ExcursionStats> completedAttempts;
    std::optional<experiments::ExcursionStats> failedAttempts;
    std::optional<experiments::QuantilePair> completionSeconds;
    std::optional<double> meanSpreadAtTriggerPoints;
    std::string excursionOrientation;  // "up" | "down"
    bool samplesTruncated = false;
};

inline void to_json(nlohmann::json& j, const ExperimentResults& r) {
    j = nlohmann::json{
        {"RUN_ID", r.RUN_ID},
        {"@timestamp", r.timestamp},
        {"hostname", r.hostname},
        {"durationSeconds", r.durationSeconds},
        {"runConfig", r.runConfig},
        {"experiment", r.experiment},
        {"occurrences", r.occurrences},
        {"ticksScanned", r.ticksScanned},
        {"daysSpanned", r.daysSpanned},
        {"occurrencesPerDay", r.occurrencesPerDay},
        {"attempts", r.attempts},
        {"failuresByLeg", r.failuresByLeg},
        {"occurrencesByMonth", r.occurrencesByMonth},
        {"occurrencesByHourUtc", r.occurrencesByHourUtc},
        {"occurrencesBySymbol", r.occurrencesBySymbol},
    };
    // Null, never a fake 0.0: a chain whose leg 1 never fired has NO
    // completion rate — 0 would read as "always fails".
    j["completionRate"] =
        r.attempts > 0
            ? nlohmann::json(static_cast<double>(r.occurrences) / r.attempts)
            : nlohmann::json(nullptr);
    // Built by hand so the model header (experimentConfig.hpp) stays
    // JSON-free — the doc shape belongs here.
    nlohmann::json perSymbol = nlohmann::json::object();
    for (const auto& [symbol, s] : r.perSymbol) {
        perSymbol[symbol] = nlohmann::json{
            {"occurrences", s.occurrences},
            {"ticksScanned", s.ticksScanned},
            {"daysSpanned", s.daysSpanned},
        };
    }
    j["perSymbol"] = std::move(perSymbol);

    // Phase-2 magnitude/timing/cost: empty populations are JSON null, never
    // fake zeros (the completionRate doctrine).
    const auto quantileJson = [](const experiments::QuantilePair& q) {
        return nlohmann::json{{"p50", q.p50}, {"p90", q.p90}};
    };
    const auto excursionJson =
        [&quantileJson](const std::optional<experiments::ExcursionStats>& s) {
            if (!s) {
                return nlohmann::json(nullptr);
            }
            return nlohmann::json{
                {"samples", s->samples},
                {"mfePoints", quantileJson(s->mfePoints)},
                {"maePoints", quantileJson(s->maePoints)},
                {"mfePercent", quantileJson(s->mfePercent)},
                {"maePercent", quantileJson(s->maePercent)},
            };
        };
    j["completedAttempts"] = excursionJson(r.completedAttempts);
    j["failedAttempts"] = excursionJson(r.failedAttempts);
    j["completionSeconds"] = r.completionSeconds
                                 ? quantileJson(*r.completionSeconds)
                                 : nlohmann::json(nullptr);
    j["meanSpreadAtTriggerPoints"] =
        r.meanSpreadAtTriggerPoints ? nlohmann::json(*r.meanSpreadAtTriggerPoints)
                                    : nlohmann::json(nullptr);
    j["excursionOrientation"] = r.excursionOrientation;
    j["samplesTruncated"] = r.samplesTruncated;
    // Top-level copies of the batch identity (also present under runConfig.*)
    // so Kibana filters don't reach into the object — appendBatchMetadata's
    // convention: absent keys, not empty strings, for pre-batch payloads.
    if (!r.runConfig.EXECUTION_TS.empty()) {
        j["executionTimestamp"] = r.runConfig.EXECUTION_TS;
    }
    if (!r.runConfig.BATCH.empty()) {
        j["batch"] = r.runConfig.BATCH;
    }
}
