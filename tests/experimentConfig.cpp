// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// Pins the experiment payload wire shapes: JSON round-trips per activity
// type, the tolerant-defaults contract (TYPE strictly required, every knob
// falling back to the struct default), the unknown-TYPE poison-pill throw,
// the run descriptor's RunConfiguration-style serializer, and the Base64
// paths the queue actually uses (JsonParser::parseExperiment*FromBase64).

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "shared/experiments/experimentConfig.hpp"
#include "shared/experiments/experimentRunConfiguration.hpp"
#include "shared/utilities/base64.hpp"
#include "shared/utilities/jsonParser.hpp"
#include "analysis/reporting/experimentResults.hpp"

using experiments::Activity;
using experiments::ActivityType;
using experiments::ExperimentConfig;
using experiments::ExperimentRunConfiguration;

namespace {

void checkActivityEqual(const Activity& restored, const Activity& original) {
    CHECK(restored.TYPE == original.TYPE);
    CHECK(restored.MOVE_PERCENT == original.MOVE_PERCENT);
    CHECK(restored.WINDOW_SECONDS == original.WINDOW_SECONDS);
    CHECK(restored.LOOKBACK_SECONDS == original.LOOKBACK_SECONDS);
    CHECK(restored.DIRECTION == original.DIRECTION);
    CHECK(restored.ATR_MULTIPLE == original.ATR_MULTIPLE);
}

}  // namespace

TEST_CASE("Activity round-trips through JSON for every type", "[experimentConfig]") {
    const Activity samples[] = {
        {.TYPE = ActivityType::DirectionalMove,
         .MOVE_PERCENT = -1.5,
         .WINDOW_SECONDS = 600},
        {.TYPE = ActivityType::StaysInBand,
         .MOVE_PERCENT = 0.5,
         .WINDOW_SECONDS = 900,
         .LOOKBACK_SECONDS = 300},
        {.TYPE = ActivityType::NewExtreme,
         .WINDOW_SECONDS = 600,
         .LOOKBACK_SECONDS = 1800,
         .DIRECTION = -1},
        {.TYPE = ActivityType::RangeRelativeMove,
         .WINDOW_SECONDS = 600,
         .LOOKBACK_SECONDS = 300,
         .DIRECTION = 1,
         .ATR_MULTIPLE = 2.5},
    };
    for (const Activity& original : samples) {
        const nlohmann::json j = original;
        INFO("TYPE = " << j.at("TYPE").get<std::string>());
        checkActivityEqual(j.get<Activity>(), original);
    }
}

TEST_CASE("Activity parses with tolerant defaults, TYPE strictly required",
          "[experimentConfig]") {
    // Only the type present: every knob falls back to the struct default.
    const auto sparse =
        nlohmann::json{{"TYPE", "StaysInBand"}}.get<Activity>();
    checkActivityEqual(sparse, Activity{.TYPE = ActivityType::StaysInBand});

    // No TYPE at all is not an activity.
    CHECK_THROWS((nlohmann::json{{"MOVE_PERCENT", 1.0}}.get<Activity>()));
}

TEST_CASE("an unknown activity TYPE throws (poison-pill path)",
          "[experimentConfig]") {
    CHECK_THROWS_AS((nlohmann::json{{"TYPE", "TeleportsSideways"}}.get<Activity>()),
                    std::invalid_argument);
    CHECK_THROWS_AS(experiments::activityTypeFromString("nonsense"),
                    std::invalid_argument);
}

TEST_CASE("ExperimentConfig round-trips its chain through JSON",
          "[experimentConfig]") {
    const ExperimentConfig original{
        .UUID = "uuid-1",
        .NAME = "dipRecovery",
        .CHAIN = {
            {.TYPE = ActivityType::DirectionalMove,
             .MOVE_PERCENT = -1.0,
             .WINDOW_SECONDS = 600},
            {.TYPE = ActivityType::DirectionalMove,
             .MOVE_PERCENT = 0.5,
             .WINDOW_SECONDS = 300},
        },
    };
    const nlohmann::json j = original;
    const auto restored = j.get<ExperimentConfig>();
    CHECK(restored.UUID == original.UUID);
    CHECK(restored.NAME == original.NAME);
    REQUIRE(restored.CHAIN.size() == original.CHAIN.size());
    for (std::size_t i = 0; i < restored.CHAIN.size(); ++i) {
        checkActivityEqual(restored.CHAIN[i], original.CHAIN[i]);
    }

    // UUID and CHAIN are strictly required; NAME is tolerant (empty default).
    CHECK_THROWS((nlohmann::json{{"NAME", "x"}}.get<ExperimentConfig>()));
    const auto unnamed = nlohmann::json{
        {"UUID", "u"},
        {"CHAIN", nlohmann::json::array()}}.get<ExperimentConfig>();
    CHECK(unnamed.NAME.empty());
}

TEST_CASE("ExperimentRunConfiguration round-trips and tolerates legacy payloads",
          "[experimentConfig]") {
    const ExperimentRunConfiguration original{
        .RUN_ID = "run-1",
        .SYMBOLS = "EURUSD,GBPUSD",
        .BATCH = "2099-01",
        .EXECUTION_TS = "2099-01-01T00:00:00Z",
        .LAST_MONTHS = 9,
        .OFFSET_MONTHS = 3,
    };
    const nlohmann::json j = original;
    const auto restored = j.get<ExperimentRunConfiguration>();
    CHECK(restored.RUN_ID == original.RUN_ID);
    CHECK(restored.SYMBOLS == original.SYMBOLS);
    CHECK(restored.BATCH == original.BATCH);
    CHECK(restored.EXECUTION_TS == original.EXECUTION_TS);
    CHECK(restored.LAST_MONTHS == original.LAST_MONTHS);
    CHECK(restored.OFFSET_MONTHS == original.OFFSET_MONTHS);

    // The optional fields fall back to the struct defaults (RunConfiguration's
    // serializer doctrine); the core three are strictly required.
    const auto minimal = nlohmann::json{
        {"RUN_ID", "r"}, {"SYMBOLS", "EURUSD"}, {"LAST_MONTHS", 6}}
                             .get<ExperimentRunConfiguration>();
    CHECK(minimal.BATCH.empty());
    CHECK(minimal.EXECUTION_TS.empty());
    CHECK(minimal.OFFSET_MONTHS == 0);
    CHECK_THROWS((nlohmann::json{{"RUN_ID", "r"}, {"SYMBOLS", "EURUSD"}}
                      .get<ExperimentRunConfiguration>()));
}

TEST_CASE("JsonParser decodes experiment payloads from Base64",
          "[experimentConfig]") {
    const ExperimentConfig experiment{
        .UUID = "uuid-64",
        .NAME = "dipRecovery",
        .CHAIN = {{.TYPE = ActivityType::NewExtreme,
                   .WINDOW_SECONDS = 600,
                   .LOOKBACK_SECONDS = 1800,
                   .DIRECTION = 1}},
    };
    const auto decoded = JsonParser::parseExperimentFromBase64(
        Base64::b64encode(nlohmann::json(experiment).dump()));
    CHECK(decoded.UUID == experiment.UUID);
    REQUIRE(decoded.CHAIN.size() == 1);
    checkActivityEqual(decoded.CHAIN[0], experiment.CHAIN[0]);

    const ExperimentRunConfiguration runConfig{
        .RUN_ID = "run-64",
        .SYMBOLS = "EURUSD",
        .BATCH = "2099-01",
        .EXECUTION_TS = "2099-01-01T00:00:00Z",
        .LAST_MONTHS = 9,
    };
    const auto decodedRun = JsonParser::parseExperimentRunFromBase64(
        Base64::b64encode(nlohmann::json(runConfig).dump()));
    CHECK(decodedRun.RUN_ID == runConfig.RUN_ID);
    CHECK(decodedRun.SYMBOLS == runConfig.SYMBOLS);
    CHECK(decodedRun.BATCH == runConfig.BATCH);
    CHECK(decodedRun.LAST_MONTHS == runConfig.LAST_MONTHS);

    // Garbage base64/JSON propagates a parse error — the drain loop's
    // poison-pill path relies on the throw, not a silent default.
    CHECK_THROWS(JsonParser::parseExperimentFromBase64("not base64 json"));
}

TEST_CASE("ExperimentResults serialises the phase-1 conditionality fields",
          "[experimentConfig]") {
    ExperimentResults results{
        .RUN_ID = "run-1",
        .timestamp = "2026-07-18T00:00:00Z",
        .hostname = "host",
        .occurrences = 3,
        .attempts = 4,
        .failuresByLeg = {0, 1},
        .occurrencesByMonth = {{"2026-01", 2}, {"2026-02", 1}},
        .perSymbol = {{"EURUSD", {.occurrences = 3,
                                  .ticksScanned = 100,
                                  .daysSpanned = 2.5}}},
        .occurrencesBySymbol = {{"EURUSD", 3}},
    };
    results.occurrencesByHourUtc[13] = 3;

    const nlohmann::json j = results;

    CHECK(j.at("attempts") == 4);
    CHECK(j.at("completionRate") == 0.75);
    REQUIRE(j.at("failuresByLeg").is_array());
    CHECK(j.at("failuresByLeg")[1] == 1);
    // Absent months are absent keys, not zeros.
    REQUIRE(j.at("occurrencesByMonth").size() == 2);
    CHECK(j.at("occurrencesByMonth").at("2026-01") == 2);
    REQUIRE(j.at("occurrencesByHourUtc").is_array());
    REQUIRE(j.at("occurrencesByHourUtc").size() == 24);
    CHECK(j.at("occurrencesByHourUtc")[13] == 3);
    // Per-symbol breakdown nests coverage; the flat v1 field is unchanged.
    CHECK(j.at("perSymbol").at("EURUSD").at("ticksScanned") == 100);
    CHECK(j.at("perSymbol").at("EURUSD").at("daysSpanned") == 2.5);
    CHECK(j.at("occurrencesBySymbol").at("EURUSD") == 3);
}

TEST_CASE("completionRate is null, never a fake zero, when nothing attempted",
          "[experimentConfig]") {
    const ExperimentResults results{.RUN_ID = "run-1"};
    const nlohmann::json j = results;
    CHECK(j.at("completionRate").is_null());
    CHECK(j.at("attempts") == 0);
    // Phase-2 empty populations follow the same doctrine.
    CHECK(j.at("completedAttempts").is_null());
    CHECK(j.at("failedAttempts").is_null());
    CHECK(j.at("completionSeconds").is_null());
    CHECK(j.at("meanSpreadAtTriggerPoints").is_null());
    CHECK(j.at("samplesTruncated") == false);
}

TEST_CASE("ExperimentResults serialises the phase-2 magnitude fields",
          "[experimentConfig]") {
    ExperimentResults results{.RUN_ID = "run-1"};
    results.completedAttempts = experiments::ExcursionStats{
        .samples = 12,
        .mfePoints = {.p50 = 495.0, .p90 = 900.0},
        .maePoints = {.p50 = 200.0, .p90 = 350.0},
        .mfePercent = {.p50 = 0.5, .p90 = 0.9},
        .maePercent = {.p50 = 0.2, .p90 = 0.35},
    };
    results.completionSeconds = experiments::QuantilePair{.p50 = 120.0, .p90 = 480.0};
    results.meanSpreadAtTriggerPoints = 20.0;
    results.excursionOrientation = "up";
    results.samplesTruncated = true;

    const nlohmann::json j = results;

    CHECK(j.at("completedAttempts").at("samples") == 12);
    CHECK(j.at("completedAttempts").at("mfePoints").at("p50") == 495.0);
    CHECK(j.at("completedAttempts").at("mfePoints").at("p90") == 900.0);
    CHECK(j.at("completedAttempts").at("maePoints").at("p90") == 350.0);
    CHECK(j.at("completedAttempts").at("maePercent").at("p50") == 0.2);
    CHECK(j.at("failedAttempts").is_null());  // populations stay separate
    CHECK(j.at("completionSeconds").at("p50") == 120.0);
    CHECK(j.at("meanSpreadAtTriggerPoints") == 20.0);
    CHECK(j.at("excursionOrientation") == "up");
    CHECK(j.at("samplesTruncated") == true);
}
