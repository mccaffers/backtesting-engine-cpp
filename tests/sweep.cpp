// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

#include <boost/decimal/literals.hpp>
#include <nlohmann/json.hpp>

#include "shared/utilities/parameterSweep.hpp"
#include "shared/tradingDefinitions/config/runConfiguration.hpp"
#include "shared/tradingDefinitions/strategy.hpp"

import randomStrategySweep;     // sweep::buildRandomStrategySweep
import runConfigurationBuilder; // sweep::makeRunConfiguration
import randomStrategy;          // RandomStrategy
import tradeManager;            // TradeManager
import priceData;               // PriceData
import trade;                   // Direction

// Pulls in the _dd user-defined literal so "5"_dd produces a decimal64_t
// directly — same convention as tradeManager.cpp.
using namespace boost::decimal::literals;

TEST_CASE("buildRandomStrategySweep registers exactly STOP and LIMIT", "[sweep]") {
    const auto generator = sweep::buildRandomStrategySweep();
    INFO("RandomStrategy sweeps exactly STOP and LIMIT pip distances");
    CHECK(generator.parameterCount() == 2);
}

// Config-agnostic: whatever STOP/LIMIT ranges the builder registers, the output
// must be their full cartesian product, so its size equals (distinct stops) x
// (distinct limits). Derived from the combinations themselves, so this holds
// for any range the sweep is tuned to (1 combo or 10,000).
TEST_CASE("buildRandomStrategySweep generates the full cartesian product", "[sweep]") {
    const auto combinations =
        sweep::buildRandomStrategySweep().generateAllCombinations();

    std::set<double> stops, limits;
    for (const auto& combo : combinations) {
        stops.insert(combo.get("STOP_DISTANCE_IN_PIPS"));
        limits.insert(combo.get("LIMIT_DISTANCE_IN_PIPS"));
    }
    CHECK(combinations.size() > 0);
    CHECK(combinations.size() == stops.size() * limits.size());
    for (const auto& combo : combinations) {
        CHECK(combo.has("STOP_DISTANCE_IN_PIPS"));
        CHECK(combo.has("LIMIT_DISTANCE_IN_PIPS"));
        // The OHLC ranges are commented out in the builder; loadCommand's
        // makeStrategy relies on has() returning false so the fields default
        // to 0 instead of throwing in get().
        CHECK_FALSE(combo.has("OHLC_COUNT"));
        CHECK_FALSE(combo.has("OHLC_MINUTES"));
    }
}

TEST_CASE("buildRandomStrategySweep covers every stop/limit pair once", "[sweep]") {
    const auto combinations =
        sweep::buildRandomStrategySweep().generateAllCombinations();

    std::set<double> stops, limits;
    std::set<std::pair<double, double>> pairs;
    for (const auto& combo : combinations) {
        const double s = combo.get("STOP_DISTANCE_IN_PIPS");
        const double l = combo.get("LIMIT_DISTANCE_IN_PIPS");
        stops.insert(s);
        limits.insert(l);
        pairs.emplace(s, l);
    }

    // No duplicate combinations, and every (stop, limit) from the grid present.
    CHECK(pairs.size() == combinations.size());
    CHECK(pairs.size() == stops.size() * limits.size());
    for (const double s : stops) {
        for (const double l : limits) {
            INFO("Missing stop=" << s << " / limit=" << l << " from the grid");
            CHECK(pairs.count({s, l}) == 1);
        }
    }
}

// generateAllCombinations must be deterministic — its order feeds the RUN
// queue, so two expansions must be byte-for-byte identical. (Config-agnostic:
// holds for any range the sweep is tuned to.)
TEST_CASE("buildRandomStrategySweep expansion order is deterministic", "[sweep]") {
    const auto a = sweep::buildRandomStrategySweep().generateAllCombinations();
    const auto b = sweep::buildRandomStrategySweep().generateAllCombinations();
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK(a[i].get("STOP_DISTANCE_IN_PIPS") == b[i].get("STOP_DISTANCE_IN_PIPS"));
        CHECK(a[i].get("LIMIT_DISTANCE_IN_PIPS") == b[i].get("LIMIT_DISTANCE_IN_PIPS"));
    }
}

TEST_CASE("makeRunConfiguration carries the run id", "[sweep]") {
    const auto config = sweep::makeRunConfiguration("test-run-id");
    CHECK(config.RUN_ID == std::string("test-run-id"));
}

TEST_CASE("makeRunConfiguration sets the run descriptor values", "[sweep]") {
    const auto config = sweep::makeRunConfiguration("test-run-id");
    CHECK(config.SYMBOLS == std::string("EURUSD"));
    CHECK(config.LAST_MONTHS == 6);
    CHECK(config.STARTING_BALANCE == tradingDefinitions::DEFAULT_STARTING_BALANCE);
    CHECK(config.MAX_LOSS_PERCENT == "5"_dd);
    CHECK(config.MAX_OPEN_TRADES == 1);
    CHECK(config.REPORT_FAILURES);
}

// The descriptor travels through Redis as JSON (loadCommand dumps it, the
// runner parses it back), so the round-trip must preserve every field —
// including the decimal ones, which move as strings via decimal_json.hpp.
TEST_CASE("makeRunConfiguration survives a JSON round-trip", "[sweep]") {
    const auto original = sweep::makeRunConfiguration("round-trip-id");

    const nlohmann::json j = original;
    const auto restored = j.get<tradingDefinitions::RunConfiguration>();

    CHECK(restored.RUN_ID == original.RUN_ID);
    CHECK(restored.SYMBOLS == original.SYMBOLS);
    CHECK(restored.LAST_MONTHS == original.LAST_MONTHS);
    CHECK(restored.STARTING_BALANCE == original.STARTING_BALANCE);
    CHECK(restored.MAX_LOSS_PERCENT == original.MAX_LOSS_PERCENT);
    CHECK(restored.MAX_OPEN_TRADES == original.MAX_OPEN_TRADES);
    CHECK(restored.REPORT_FAILURES == original.REPORT_FAILURES);
}

// RandomStrategy ignores everything in its config, so default-constructed
// Strategy is enough — selectStrategy only needs the name, and these tests
// construct the class directly.
TEST_CASE("RandomStrategy always returns a signal", "[sweep]") {
    RandomStrategy strategy{tradingDefinitions::Strategy{}};
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");

    for (int i = 0; i < 100; ++i) {
        const auto signal = strategy.decide(tick);
        REQUIRE(signal.has_value());
        CHECK((*signal == Direction::LONG || *signal == Direction::SHORT));
    }
}

// A fair coin over 1000 flips produces both directions with probability
// 1 - 2^-999 — a single-sided run means the distribution (or seeding) broke.
TEST_CASE("RandomStrategy produces both directions", "[sweep]") {
    RandomStrategy strategy{tradingDefinitions::Strategy{}};
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");

    bool sawLong = false;
    bool sawShort = false;
    for (int i = 0; i < 1000 && !(sawLong && sawShort); ++i) {
        const auto signal = strategy.decide(tick);
        sawLong = sawLong || (signal == Direction::LONG);
        sawShort = sawShort || (signal == Direction::SHORT);
    }
    CHECK(sawLong);
    CHECK(sawShort);
}

// Exits are owned by Operations via SL/TP — during() must not touch open
// positions, otherwise the sweep's stop/limit parameters stop being the only
// exit mechanism under test.
TEST_CASE("RandomStrategy::during leaves open trades alone", "[sweep]") {
    RandomStrategy strategy{tradingDefinitions::Strategy{}};
    TradeManager manager;
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick, 1, Direction::LONG);

    strategy.during(tick, manager);

    CHECK(manager.getActiveTrades().size() == 1);
    CHECK(manager.getClosedTrades().size() == 0);
}
