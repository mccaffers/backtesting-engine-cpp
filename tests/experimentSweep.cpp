// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// Pins the experiment sweep MACHINERY, not the tuned grid values: the grid
// expands to the product of its range sizes, the factory's combination ->
// chain mapping (signs, minutes -> seconds), and buildExperimentChunk's
// key/payload contract — all without Redis.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "shared/experiments/experimentConfig.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "load/redisLoader.hpp"

import experimentsCommand;  // sweep::buildExperimentChunk (+ re-exported
                            // dipRecoverySweep: ExperimentSweepSpec, factory)
import makeDipRecovery;     // sweep::makeDipRecoveryExperiment

using experiments::ActivityType;

TEST_CASE("buildDipRecoverySweep expands to the product of its range sizes",
          "[experimentSweep]") {
    const auto spec = sweep::buildDipRecoverySweep();
    const auto counts = spec.generator.rangeValueCounts();
    REQUIRE(!counts.empty());

    // Config-agnostic: however the grid is tuned, its size must be the
    // product of the declared range sizes.
    std::size_t product = 1;
    for (const auto& [name, count] : counts) {
        INFO(name << " declares no values");
        CHECK(count > 0);
        product *= count;
    }
    CHECK(spec.generator.combinationCount() == product);

    // Every combination carries every registered range by construction, so
    // probing the two grid ends proves the four leg-prefixed names are
    // registered (the factory reads them with no has() fallback).
    for (const auto& combo :
         {spec.generator.combinationAt(0),
          spec.generator.combinationAt(spec.generator.combinationCount() - 1)}) {
        for (const char* name : {"LEG1_DROP_PERCENT", "LEG1_WINDOW_MINUTES",
                                 "LEG2_RISE_PERCENT", "LEG2_WINDOW_MINUTES"}) {
            INFO("Combination is missing " << name);
            CHECK(combo.has(name));
        }
    }

    // The per-sweep history window (colocated with the grid by design).
    CHECK(spec.lastMonths == 9);
    CHECK(spec.offsetMonths == 0);
    CHECK(spec.factory == sweep::makeDipRecoveryExperiment);
}

TEST_CASE("makeDipRecoveryExperiment maps a combination onto the chain",
          "[experimentSweep]") {
    sweep::Combination combo;
    combo.set("LEG1_DROP_PERCENT", 1.0);
    combo.set("LEG1_WINDOW_MINUTES", 10);
    combo.set("LEG2_RISE_PERCENT", 0.5);
    combo.set("LEG2_WINDOW_MINUTES", 30);

    const auto config = sweep::makeDipRecoveryExperiment(combo);

    CHECK_FALSE(config.UUID.empty());
    CHECK(config.NAME == "dipRecovery");
    REQUIRE(config.CHAIN.size() == 2);

    // Leg 1: the drop — the grid sweeps a positive magnitude, the factory
    // owns the sign; minutes become WINDOW_SECONDS.
    CHECK(config.CHAIN[0].TYPE == ActivityType::DirectionalMove);
    CHECK(config.CHAIN[0].MOVE_PERCENT == -1.0);
    CHECK(config.CHAIN[0].WINDOW_SECONDS == 10 * 60);

    // Leg 2: the recovery — positive, its own window.
    CHECK(config.CHAIN[1].TYPE == ActivityType::DirectionalMove);
    CHECK(config.CHAIN[1].MOVE_PERCENT == 0.5);
    CHECK(config.CHAIN[1].WINDOW_SECONDS == 30 * 60);

    // Each call mints a FRESH UUID — payload keys must never collide.
    CHECK(sweep::makeDipRecoveryExperiment(combo).UUID != config.UUID);
}

TEST_CASE("experiment queue keys embed run id and experiment uuid",
          "[experimentSweep]") {
    CHECK(queue_keys::experimentKey("run-1") ==
          std::string("BACKTESTING_QUEUE_EXPERIMENT:run-1"));
    CHECK(queue_keys::experimentPayloadKey("run-1", "uuid-2") ==
          std::string("BACKTESTING_QUEUE_EXPERIMENT_PAYLOAD:run-1:uuid-2"));
}

// buildExperimentChunk is the producer's streaming seam — buildStrategyChunk's
// exact contract: 1:1 with the lazily-decoded combinations in index order,
// every key derived from the run id and the payload's OWN freshly-minted
// UUID, and the JSON round-trips to an ExperimentConfig carrying that
// combination's swept values.
TEST_CASE("buildExperimentChunk keys each payload by run id and its UUID",
          "[experimentSweep]") {
    const auto spec = sweep::buildDipRecoverySweep();
    const std::string runId = "test-run-id";

    const std::size_t begin = 5;
    const std::size_t end = 12;
    const auto chunk = sweep::buildExperimentChunk(spec.generator, spec.factory,
                                                   runId, begin, end);
    REQUIRE(chunk.size() == end - begin);

    std::set<std::string> uuids;
    for (std::size_t offset = 0; offset < chunk.size(); ++offset) {
        const auto config = nlohmann::json::parse(chunk[offset].rawJson)
                                .get<experiments::ExperimentConfig>();
        CHECK_FALSE(config.UUID.empty());
        uuids.insert(config.UUID);
        CHECK(chunk[offset].key ==
              queue_keys::experimentPayloadKey(runId, config.UUID));

        // The payload parses back to the factory's mapping of the SAME grid
        // index (UUIDs aside — those are minted per call).
        const auto expected =
            spec.factory(spec.generator.combinationAt(begin + offset));
        CHECK(config.NAME == expected.NAME);
        REQUIRE(config.CHAIN.size() == expected.CHAIN.size());
        for (std::size_t leg = 0; leg < config.CHAIN.size(); ++leg) {
            CHECK(config.CHAIN[leg].TYPE == expected.CHAIN[leg].TYPE);
            CHECK(config.CHAIN[leg].MOVE_PERCENT ==
                  expected.CHAIN[leg].MOVE_PERCENT);
            CHECK(config.CHAIN[leg].WINDOW_SECONDS ==
                  expected.CHAIN[leg].WINDOW_SECONDS);
        }
    }
    CHECK(uuids.size() == chunk.size());  // no key collisions inside a chunk

    // An empty range (how the stream terminates) yields an empty chunk.
    CHECK(sweep::buildExperimentChunk(spec.generator, spec.factory, runId, end,
                                      end)
              .empty());
}
