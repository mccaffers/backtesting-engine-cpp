// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstdlib>  // setenv/unsetenv — the batch label is env-overridable
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "run/reporting/outcomeIndices.hpp"
#include "run/reporting/tradingResults.hpp"
#include "shared/tradingDefinitions/config/configuration.hpp"

import elasticClient;  // resultsBaseFor — the winners/results routing split
import rollingWindow;  // rolling::kFullHistory — the ladder's terminal window

// Fixed epochs, each verified against strftime("%G-%V") itself
// (date -u -r <epoch> +%G-%V): the ISO week-based year differs from the
// calendar year around New Year, and the week is zero-padded so lexical
// order stays chronological.
TEST_CASE("isoWeekLabel formats the ISO year and week", "[outcomeIndices]") {
    CHECK(outcome_index::isoWeekLabel(1783771200) == "2026-28");  // 2026-07-11
    CHECK(outcome_index::isoWeekLabel(1767268800) == "2026-01");  // 2026-01-01 (zero-padded)
    CHECK(outcome_index::isoWeekLabel(1735560000) == "2025-01");  // 2024-12-30 (ISO year ahead)
    CHECK(outcome_index::isoWeekLabel(1798804800) == "2026-53");  // 2027-01-01 (ISO year behind)
}

// $BACKTEST_BATCH pins the label (re-runs; load.sh exports it so its eight
// per-strategy invocations can't straddle an ISO-week boundary); without it
// the label is the current UTC week in the same YYYY-WW shape.
TEST_CASE("currentBatchLabel prefers the env override", "[outcomeIndices]") {
    setenv("BACKTEST_BATCH", "2099-01", 1);
    CHECK(outcome_index::currentBatchLabel() == "2099-01");

    unsetenv("BACKTEST_BATCH");
    const std::string label = outcome_index::currentBatchLabel();
    REQUIRE(label.size() == 7);
    for (std::size_t i = 0; i < label.size(); ++i) {
        INFO("unexpected character at index " << i << " in " << label);
        if (i == 4) {
            CHECK(label[i] == '-');
        } else {
            CHECK(std::isdigit(static_cast<unsigned char>(label[i])) != 0);
        }
    }
}

TEST_CASE("weeklyIndex suffixes the batch and falls back bare", "[outcomeIndices]") {
    CHECK(outcome_index::weeklyIndex(outcome_index::kResultsBase, "2026-28") ==
          "backtesting-results-2026-28");
    // Empty batch (payloads predating the field, hand-run configs) keeps
    // writing to the unsuffixed base — the legacy escape hatch.
    CHECK(outcome_index::weeklyIndex(outcome_index::kResultsBase, "") ==
          "backtesting-results");
}

TEST_CASE("currentAlias derives the rolling alias name", "[outcomeIndices]") {
    CHECK(outcome_index::currentAlias(outcome_index::kWinnersBase) ==
          "backtesting-winners-current");
    // Elasticsearch forbids an alias sharing a concrete index's name, and the
    // unsuffixed fallback index is the one name the batch suffix can't
    // distinguish the alias from.
    for (const std::string_view base : outcome_index::kWeeklyBases) {
        CHECK(outcome_index::currentAlias(base) !=
              outcome_index::weeklyIndex(base, ""));
    }
}

// The winners/results split: exactly the ladder's terminal full-history
// window routes to the winners index — the population live boots against —
// and every other window is a screening pass. Derived from
// rolling::kFullHistory rather than literals, so the test follows the ladder
// if it ever grows.
TEST_CASE("resultsBaseFor routes only the terminal window to the winners index",
          "[outcomeIndices]") {
    const rolling::Window full = rolling::kFullHistory;
    CHECK(resultsBaseFor(full.lastMonths, full.offsetMonths) ==
          outcome_index::kWinnersBase);
    CHECK(resultsBaseFor(full.lastMonths, full.offsetMonths + 3) ==
          outcome_index::kResultsBase);
    CHECK(resultsBaseFor(full.lastMonths - 1, full.offsetMonths) ==
          outcome_index::kResultsBase);
    CHECK(resultsBaseFor(0, 0) == outcome_index::kResultsBase);
}

// Outcome documents carry top-level copies of the batch identity for Kibana
// filters/aggregations (the embedded config has them too) — and a pre-batch
// config keeps its exact legacy shape: absent keys, not empty strings.
TEST_CASE("outcome documents carry the batch identity when present",
          "[outcomeIndices]") {
    tradingDefinitions::Configuration config;
    config.RUN_ID = "run";
    config.BATCH = "2099-01";
    config.EXECUTION_TS = "2099-01-01T00:00:00Z";

    const TradeFinal finalDoc{.RUN_ID = "run",
                              .timestamp = "t",
                              .durationSeconds = 1.0,
                              .success = 1,
                              .status = "completed",
                              .hostname = "host",
                              .config = config};
    const nlohmann::json finalJson = finalDoc;
    CHECK(finalJson.at("batch") == "2099-01");
    CHECK(finalJson.at("executionTimestamp") == "2099-01-01T00:00:00Z");
    CHECK(finalJson.at("config").at("BATCH") == "2099-01");

    const TradingFailure failureDoc{.RUN_ID = "run",
                                    .timestamp = "t",
                                    .durationSeconds = 1.0,
                                    .reason = "loss_limit",
                                    .breachPnlPips = 0.0,
                                    .lossFloorPips = 0.0,
                                    .hostname = "host",
                                    .config = config,
                                    .results = {}};
    const nlohmann::json failureJson = failureDoc;
    CHECK(failureJson.at("batch") == "2099-01");
    CHECK(failureJson.at("executionTimestamp") == "2099-01-01T00:00:00Z");

    config.BATCH.clear();
    config.EXECUTION_TS.clear();
    const TradingResults legacyDoc{.RUN_ID = "run",
                                   .timestamp = "t",
                                   .durationSeconds = 1.0,
                                   .hostname = "host",
                                   .config = config,
                                   .results = {}};
    const nlohmann::json legacyJson = legacyDoc;
    CHECK_FALSE(legacyJson.contains("batch"));
    CHECK_FALSE(legacyJson.contains("executionTimestamp"));
}
