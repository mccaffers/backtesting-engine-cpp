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
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Intentionally a plain header, NOT a .cppm module — see runConfiguration.hpp
// for why JSON-serializable structs stay GMF-includable headers in this repo.
//
// The experiment model: a chain of "activities" asked of the tick history —
// "price drops 1% over 10 minutes, then rises 0.5% in a further 10 minutes:
// how many times does that occur?" — without writing a full strategy. The
// producer (experimentsCommand) spans a parameter grid of these out to Redis;
// the analysis worker replays them against QuestDB ticks via chainMatcher.
namespace experiments {

// The primitive an activity tests. Kept deliberately small for v1:
//  - DirectionalMove:   price moves MOVE_PERCENT (signed: negative = drop)
//                       within WINDOW_SECONDS.
//  - StaysInBand:       the trailing band window's mid range stays within
//                       MOVE_PERCENT (total width, sign ignored).
//  - NewExtreme:        price strictly exceeds the trailing LOOKBACK_SECONDS
//                       high (DIRECTION +1) or low (-1).
//  - RangeRelativeMove: price moves ATR_MULTIPLE x the trailing
//                       LOOKBACK_SECONDS high-low range ("ATR"-style, but
//                       deque-tracked tick range, not bar ATR — see
//                       chainMatcher) in DIRECTION within WINDOW_SECONDS.
enum class ActivityType {
    DirectionalMove,
    StaysInBand,
    NewExtreme,
    RangeRelativeMove,
};

inline std::string toString(const ActivityType type) {
    switch (type) {
        case ActivityType::DirectionalMove: return "DirectionalMove";
        case ActivityType::StaysInBand: return "StaysInBand";
        case ActivityType::NewExtreme: return "NewExtreme";
        case ActivityType::RangeRelativeMove: return "RangeRelativeMove";
    }
    throw std::invalid_argument("experiments::toString: invalid ActivityType");
}

// Unknown names throw — a payload naming a type this binary doesn't know is a
// poison pill the worker must retire loudly, never misread as a default.
inline ActivityType activityTypeFromString(const std::string& name) {
    if (name == "DirectionalMove") return ActivityType::DirectionalMove;
    if (name == "StaysInBand") return ActivityType::StaysInBand;
    if (name == "NewExtreme") return ActivityType::NewExtreme;
    if (name == "RangeRelativeMove") return ActivityType::RangeRelativeMove;
    throw std::invalid_argument(
        "experiments::activityTypeFromString: unknown ActivityType '" + name +
        "'");
}

// One leg of the chain. Deliberately FLAT (every primitive's knobs in one
// struct, unused ones left at their defaults) so the sweep's leg-prefixed
// parameter names (LEG1_MOVE_PERCENT, ...) map on without any per-type
// machinery — mirroring how Combination is a flat map<string,double>.
// Which fields each TYPE reads is validated by ChainMatcher's ctor.
struct Activity {
    ActivityType TYPE = ActivityType::DirectionalMove;
    double MOVE_PERCENT = 0.0;   // signed for DirectionalMove; width for band
    int WINDOW_SECONDS = 0;      // completion window (and leg-1 trailing window)
    int LOOKBACK_SECONDS = 0;    // trailing basis window (band/extreme/range)
    int DIRECTION = 0;           // +1 / -1 for NewExtreme & RangeRelativeMove
    double ATR_MULTIPLE = 0.0;   // RangeRelativeMove threshold multiplier
};

// Hand-written (house style): TYPE strictly required — an activity without a
// type is meaningless — while every knob falls back to the struct default so
// payloads only carry the fields their primitive reads.
inline void to_json(nlohmann::json& j, const Activity& a) {
    j = nlohmann::json{
        {"TYPE", toString(a.TYPE)},
        {"MOVE_PERCENT", a.MOVE_PERCENT},
        {"WINDOW_SECONDS", a.WINDOW_SECONDS},
        {"LOOKBACK_SECONDS", a.LOOKBACK_SECONDS},
        {"DIRECTION", a.DIRECTION},
        {"ATR_MULTIPLE", a.ATR_MULTIPLE},
    };
}

inline void from_json(const nlohmann::json& j, Activity& a) {
    a.TYPE = activityTypeFromString(j.at("TYPE").get<std::string>());
    const Activity defaults{};
    a.MOVE_PERCENT = j.value("MOVE_PERCENT", defaults.MOVE_PERCENT);
    a.WINDOW_SECONDS = j.value("WINDOW_SECONDS", defaults.WINDOW_SECONDS);
    a.LOOKBACK_SECONDS = j.value("LOOKBACK_SECONDS", defaults.LOOKBACK_SECONDS);
    a.DIRECTION = j.value("DIRECTION", defaults.DIRECTION);
    a.ATR_MULTIPLE = j.value("ATR_MULTIPLE", defaults.ATR_MULTIPLE);
}

// One experiment: an ordered chain of activities counted non-overlapping
// against the tick stream. UUID is minted by the sweep factory (the payload
// key and the Elasticsearch _id both build on it); NAME is the sweep's name
// ("dipRecovery") echoed into the results doc for Kibana filtering.
struct ExperimentConfig {
    std::string UUID;
    std::string NAME;
    std::vector<Activity> CHAIN;
};

inline void to_json(nlohmann::json& j, const ExperimentConfig& c) {
    j = nlohmann::json{
        {"UUID", c.UUID},
        {"NAME", c.NAME},
        {"CHAIN", c.CHAIN},
    };
}

inline void from_json(const nlohmann::json& j, ExperimentConfig& c) {
    j.at("UUID").get_to(c.UUID);
    j.at("CHAIN").get_to(c.CHAIN);
    c.NAME = j.value("NAME", std::string{});
}

// Nearest-rank percentiles over an exact (possibly capped — see
// samplesTruncated) sample population.
struct QuantilePair {
    double p50 = 0.0;
    double p90 = 0.0;
};

// Excursion percentiles for one attempt population (completed and failed are
// kept SEPARATE — the failed-attempt MAE is the stop-loss question, the
// completed-attempt MFE is the take-profit question). MFE/MAE are oriented
// by the final leg's implied direction (ExperimentOutcome's
// excursionOrientation); percent variants are relative to each attempt's own
// leg-1 anchor mid.
struct ExcursionStats {
    std::uint64_t samples = 0;
    QuantilePair mfePoints;
    QuantilePair maePoints;
    QuantilePair mfePercent;
    QuantilePair maePercent;
};

// One symbol's slice of an experiment outcome. Per-symbol tick counts and
// spans matter because the UNION-ALL stream's global numbers hide a sparsely
// ticked symbol — its rate would look artificially low against the shared
// denominator.
struct SymbolOutcome {
    std::uint64_t occurrences = 0;
    std::uint64_t ticksScanned = 0;
    double daysSpanned = 0.0;
};

// What one experiment's replay over one symbol group's ticks produced —
// returned by chain_matcher::evaluateExperiment and echoed into the results
// document. Lives here (plain header, no JSON — the doc shape belongs to
// experimentResults.hpp) so the analysis worker's textual drain loop can name
// it without importing the chainMatcher module.
struct ExperimentOutcome {
    std::uint64_t occurrences = 0;
    std::uint64_t ticksScanned = 0;
    double daysSpanned = 0.0;  // max - min tick timestamp, in days
    // Leg-1 completions (anchors) summed across symbols — the denominator
    // for P(chain | leg 1); serial single-anchor, so conservative in
    // clustered periods (see chainMatcher's module comment).
    std::uint64_t attempts = 0;
    // Chain deaths attributed to the leg being sought (index 0 stays 0),
    // summed across symbols. Sized to the chain length.
    std::vector<std::uint64_t> failuresByLeg;
    // Completion counts bucketed by the completing tick's UTC calendar slot:
    // regime stability ("YYYY-MM" keys — absent months are absent keys, not
    // zeros) and session dependence (hour-of-day histogram).
    std::map<std::string, std::uint64_t> occurrencesByMonth;
    std::array<std::uint64_t, 24> occurrencesByHourUtc{};
    // Per-symbol breakdown; the flat map below is the v1 shape, kept so
    // existing documents and dashboards keep their field.
    std::map<std::string, SymbolOutcome> perSymbol;
    std::map<std::string, std::uint64_t> occurrencesBySymbol;
    // Magnitude/timing/cost (phase 2). nullopt = empty population (the doc
    // then reads null, never fake zeros — the completionRate doctrine).
    // Excursions are measured vs each attempt's leg-1 anchor and oriented by
    // the FINAL leg's implied direction: "up" = above-anchor counts as MFE,
    // "down" = below-anchor does (StaysInBand finals default to "up").
    std::optional<ExcursionStats> completedAttempts;
    std::optional<ExcursionStats> failedAttempts;
    std::optional<QuantilePair> completionSeconds;  // anchor -> chain done
    std::optional<double> meanSpreadAtTriggerPoints;  // ask-bid at anchors
    std::string excursionOrientation;  // "up" | "down"
    // Quantiles are exact until kMaxAttemptSamples per matcher, then samples
    // stop while COUNTS stay exact — flagged so a truncated percentile is
    // never mistaken for a full-population one.
    bool samplesTruncated = false;
};

}  // namespace experiments
