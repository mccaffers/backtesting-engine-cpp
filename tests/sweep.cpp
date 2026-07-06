// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/decimal/literals.hpp>
#include <nlohmann/json.hpp>

#include "shared/tradingDefinitions/config/runConfiguration.hpp"
#include "shared/tradingDefinitions/strategyConfig.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "load/redisLoader.hpp"

import loadCommand;             // sweep::buildStrategyChunk, sweep::StrategyFactory
import makeStrategy;            // sweep::makeStrategy
import parameterGenerator;      // sweep::ParameterGenerator, sweep::Combination
import randomStrategySweep;     // sweep::buildRandomStrategySweep
import ohlcBreakoutStrategySweep; // sweep::buildOhlcBreakoutStrategySweep
import makeOhlcBreakoutStrategy;  // sweep::makeOhlcBreakoutStrategy
import runConfigurationBuilder; // sweep::makeRunConfiguration, sweep::resolveSymbolGroups
import symbolGroups;            // sweep::cleanSymbols, sweep::allSymbolsKnown
import randomStrategy;          // RandomStrategy
import ohlcBreakoutStrategy;    // OhlcBreakoutStrategy
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
    const auto config = sweep::makeRunConfiguration("test-run-id", "EURUSD");
    CHECK(config.RUN_ID == std::string("test-run-id"));
}

TEST_CASE("makeRunConfiguration sets the run descriptor values", "[sweep]") {
    const auto config = sweep::makeRunConfiguration("test-run-id", "EURUSD");
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
    const auto original = sweep::makeRunConfiguration("round-trip-id", "EURUSD");

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

// cleanSymbols normalises one symbol group for the run side, which splits
// SYMBOLS on ',' WITHOUT trimming — so surrounding whitespace and empty fields
// must be stripped here, while a multi-instrument group stays comma-joined.
TEST_CASE("cleanSymbols trims whitespace and drops empty fields", "[sweep]") {
    CHECK(sweep::cleanSymbols("EURUSD") == std::string("EURUSD"));
    CHECK(sweep::cleanSymbols("EURUSD,AUDUSD") == std::string("EURUSD,AUDUSD"));
    CHECK(sweep::cleanSymbols("EURUSD, AUDUSD") == std::string("EURUSD,AUDUSD"));
    CHECK(sweep::cleanSymbols("  EURUSD ,\tAUDUSD  ") == std::string("EURUSD,AUDUSD"));
    CHECK(sweep::cleanSymbols("EURUSD,,AUDUSD,") == std::string("EURUSD,AUDUSD"));
    CHECK(sweep::cleanSymbols("   ").empty());
}

// allSymbolsKnown backs the static_asserts guarding kSymbolGroups and the
// per-sweep overrides: it must tokenise groups exactly like cleanSymbols
// (trim, drop empties) before the symbol_scale lookup, and reject any symbol
// missing from the table.
TEST_CASE("allSymbolsKnown validates groups against symbol_scale", "[sweep]") {
    constexpr auto known = std::to_array<std::string_view>(
        {"EURUSD", " GBPUSD ,\tAUDUSD", "XAUUSD,,"});
    constexpr auto unknown = std::to_array<std::string_view>(
        {"EURUSD", "EURUSD,NOTASYMBOL"});
    CHECK(sweep::allSymbolsKnown(known));
    CHECK_FALSE(sweep::allSymbolsKnown(unknown));
    CHECK(sweep::allSymbolsKnown({}));  // no groups = nothing to reject
}

// resolveSymbolGroups picks the sweep's own symbol list when it set one and
// falls back to the full kSymbolGroups default otherwise — the load command
// fans out one run per returned group, so this decides what a `load` targets.
TEST_CASE("resolveSymbolGroups prefers the sweep override", "[sweep]") {
    const std::vector<std::string> overrideGroups{"EURUSD", "GBPUSD,AUDUSD"};
    CHECK(sweep::resolveSymbolGroups(overrideGroups) == overrideGroups);

    const auto defaults = sweep::resolveSymbolGroups({});
    REQUIRE(defaults.size() == sweep::kSymbolGroups.size());
    for (std::size_t i = 0; i < defaults.size(); ++i) {
        CHECK(defaults[i] == sweep::kSymbolGroups[i]);
    }
}

// The shipped kSymbolGroupsOverride narrows the sweep to EURUSD — widening it
// back to the full kSymbolGroups set is a deliberate source edit in
// randomStrategySweep (empty the override), never a silent default.
TEST_CASE("buildRandomStrategySweep narrows the symbols to EURUSD", "[sweep]") {
    CHECK(sweep::buildRandomStrategySweep().symbolGroups() ==
          std::vector<std::string>{"EURUSD"});
}

// RandomStrategy ignores everything in its config, so default-constructed
// StrategyConfig is enough — selectStrategy only needs the name, and these tests
// construct the class directly.
TEST_CASE("RandomStrategy always returns a signal", "[sweep]") {
    RandomStrategy strategy{tradingDefinitions::StrategyConfig{}};
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
    RandomStrategy strategy{tradingDefinitions::StrategyConfig{}};
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
    RandomStrategy strategy{tradingDefinitions::StrategyConfig{}};
    TradeManager manager;
    PriceData tick(110010, 110000, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick, 1, Direction::LONG);

    strategy.during(tick, manager);

    CHECK(manager.getActiveTrades().size() == 1);
    CHECK(manager.getClosedTrades().size() == 0);
}

// combinationCount/combinationAt are the lazy mirror of generateAllCombinations
// — the breakout tests below lean on them because that grid is billions of
// combinations, far too large to materialise. Pin the lazy decode to the eager
// expansion on a small synthetic grid: same size, index-for-index identical.
TEST_CASE("ParameterGenerator lazy accessors mirror generateAllCombinations", "[sweep]") {
    sweep::ParameterGenerator generator;
    generator.addList("A", {1, 3, 5});
    generator.addRange("B", 10, 10, 20);     // 10, 20
    generator.addExponential("C", 2, 3, 2);  // 2, 6

    const auto combinations = generator.generateAllCombinations();
    REQUIRE(combinations.size() == 3 * 2 * 2);
    REQUIRE(generator.combinationCount() == combinations.size());
    for (std::size_t i = 0; i < combinations.size(); ++i) {
        INFO("Lazy decode diverges from eager expansion at index " << i);
        CHECK(generator.combinationAt(i).values() == combinations[i].values());
    }

    const auto counts = generator.rangeValueCounts();
    REQUIRE(counts.size() == 3);
    CHECK(counts[0] == std::pair<std::string, std::size_t>{"A", 3});
    CHECK(counts[1] == std::pair<std::string, std::size_t>{"B", 2});
    CHECK(counts[2] == std::pair<std::string, std::size_t>{"C", 2});
}

// Key shapes for the split queue: the per-run list carries payload key names,
// each payload lives under its own key grouped by RUN_ID for SCAN-able cleanup.
TEST_CASE("queue keys embed run id and strategy uuid", "[sweep]") {
    CHECK(queue_keys::strategyKey("run-1") ==
          std::string("BACKTESTING_QUEUE_STRATEGY:run-1"));
    CHECK(queue_keys::strategyPayloadKey("run-1", "uuid-2") ==
          std::string("BACKTESTING_QUEUE_STRATEGY_PAYLOAD:run-1:uuid-2"));
}

// buildStrategyChunk is the producer's streaming seam: it maps grid indices
// [begin, end) onto (payload key, strategy JSON) pairs. Contract pinned here:
// 1:1 with the lazily-decoded combinations in index order, every key derived
// from the run id and the payload's OWN freshly-minted UUID, and the JSON
// round-trips to a StrategyConfig carrying that combination's swept values.
TEST_CASE("buildStrategyChunk keys each payload by run id and its UUID", "[sweep]") {
    const auto generator = sweep::buildRandomStrategySweep();
    const std::string runId = "test-run-id";

    const std::size_t begin = 5;
    const std::size_t end = 12;
    const auto chunk =
        sweep::buildStrategyChunk(generator, sweep::makeStrategy, runId, begin, end);
    REQUIRE(chunk.size() == end - begin);

    for (std::size_t offset = 0; offset < chunk.size(); ++offset) {
        const auto config = nlohmann::json::parse(chunk[offset].rawJson)
                                .get<tradingDefinitions::StrategyConfig>();
        CHECK_FALSE(config.UUID.empty());
        CHECK(chunk[offset].key ==
              queue_keys::strategyPayloadKey(runId, config.UUID));

        const auto combo = generator.combinationAt(begin + offset);
        CHECK(config.TRADING_VARIABLES.STOP_DISTANCE_IN_PIPS ==
              static_cast<int>(combo.get("STOP_DISTANCE_IN_PIPS")));
        CHECK(config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_PIPS ==
              static_cast<int>(combo.get("LIMIT_DISTANCE_IN_PIPS")));
    }

    // An empty range (how the stream terminates) yields an empty chunk.
    CHECK(sweep::buildStrategyChunk(generator, sweep::makeStrategy, runId, end, end)
              .empty());
}

// Names the OhlcBreakout sweep registers and makeOhlcBreakoutStrategy reads
// back; the two must stay in lockstep or the mapper throws at load time.
namespace {
const std::array<std::string, 7> kBreakoutParameterNames = {
    "BREAKOUT_OHLC_MINUTES", "BREAKOUT_OHLC_COUNT",
    "TREND_OHLC_MINUTES",    "TREND_OHLC_COUNT",
    "BUFFER_PIPS",           "STOP_DISTANCE_IN_PIPS",
    "LIMIT_DISTANCE_IN_PIPS"};

// Stride of each parameter's axis in expansion order (later-registered ranges
// vary fastest): stepping combinationAt's index by strides[k] walks parameter
// k through its values while every other parameter holds still.
std::vector<std::size_t> axisStrides(
    const std::vector<std::pair<std::string, std::size_t>>& counts) {
    std::vector<std::size_t> strides(counts.size(), 1);
    for (std::size_t k = counts.size(); k >= 2; --k) {
        strides[k - 2] = strides[k - 1] * counts[k - 1].second;
    }
    return strides;
}
}

TEST_CASE("buildOhlcBreakoutStrategySweep registers all seven parameters", "[sweep]") {
    const auto generator = sweep::buildOhlcBreakoutStrategySweep();
    CHECK(generator.parameterCount() == kBreakoutParameterNames.size());

    // Every combination carries every registered range by construction, so
    // probing the two ends of the grid proves the names are registered without
    // materialising the billions of combinations in between.
    REQUIRE(generator.combinationCount() > 0);
    for (const auto& combo : {generator.combinationAt(0),
                              generator.combinationAt(generator.combinationCount() - 1)}) {
        for (const auto& name : kBreakoutParameterNames) {
            INFO("Combination is missing " << name);
            CHECK(combo.has(name));
        }
    }
}

// Config-agnostic (same idea as the random product test), but checked through
// the lazy API because the breakout grid is too large to materialise: walking
// each parameter's axis must yield that range's declared number of DISTINCT
// values (a duplicate inside a range would shrink the true product), their
// product must equal combinationCount(), and a strided sample of full
// combinations must contain no duplicates.
TEST_CASE("buildOhlcBreakoutStrategySweep generates the full cartesian product", "[sweep]") {
    const auto generator = sweep::buildOhlcBreakoutStrategySweep();
    const auto counts = generator.rangeValueCounts();
    const auto total = generator.combinationCount();
    REQUIRE(total > 0);
    REQUIRE(!counts.empty());

    const auto strides = axisStrides(counts);
    std::size_t product = 1;
    for (std::size_t k = 0; k < counts.size(); ++k) {
        std::set<double> axisValues;
        for (std::size_t j = 0; j < counts[k].second; ++j) {
            axisValues.insert(generator.combinationAt(j * strides[k]).get(counts[k].first));
        }
        INFO(counts[k].first << " expands to duplicate values");
        CHECK(axisValues.size() == counts[k].second);
        product *= axisValues.size();
    }
    CHECK(product == total);

    constexpr std::size_t kSamples = 1000;
    const std::size_t sampleStride = total > kSamples ? total / kSamples : 1;
    std::set<std::map<std::string, double>> uniqueCombinations;
    std::size_t sampled = 0;
    for (std::size_t i = 0; i < total; i += sampleStride, ++sampled) {
        uniqueCombinations.insert(generator.combinationAt(i).values());
    }
    CHECK(uniqueCombinations.size() == sampled);
}

// The breakout sweep narrows itself to EURUSD while the strategy is being
// validated (kOhlcBreakoutSymbolGroupsOverride) — widening it is a deliberate
// source edit, never a silent default.
TEST_CASE("buildOhlcBreakoutStrategySweep narrows the symbols to EURUSD", "[sweep]") {
    const auto generator = sweep::buildOhlcBreakoutStrategySweep();
    CHECK(generator.symbolGroups() == std::vector<std::string>{"EURUSD"});
}

TEST_CASE("makeOhlcBreakoutStrategy maps a combination onto the config", "[sweep]") {
    sweep::Combination combo;
    combo.set("BREAKOUT_OHLC_MINUTES", 15);
    combo.set("BREAKOUT_OHLC_COUNT", 24);
    combo.set("TREND_OHLC_MINUTES", 60);
    combo.set("TREND_OHLC_COUNT", 50);
    combo.set("BUFFER_PIPS", 5);
    combo.set("STOP_DISTANCE_IN_PIPS", 40);
    combo.set("LIMIT_DISTANCE_IN_PIPS", 60);

    const auto config = sweep::makeOhlcBreakoutStrategy(combo);

    // Must match the dispatch string in run/operations.cppm.
    CHECK(config.TRADING_VARIABLES.STRATEGY == "OhlcBreakoutStrategy");
    CHECK(config.TRADING_VARIABLES.STOP_DISTANCE_IN_PIPS == 40);
    CHECK(config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_PIPS == 60);
    CHECK(config.TRADING_VARIABLES.TRADING_SIZE == 1);
    CHECK_FALSE(config.UUID.empty());

    // Positional contract: [0] breakout timeframe, [1] trend timeframe.
    REQUIRE(config.OHLC_VARIABLES.size() == 2);
    CHECK(config.OHLC_VARIABLES[0].OHLC_COUNT == 24);
    CHECK(config.OHLC_VARIABLES[0].OHLC_MINUTES == 15);
    CHECK(config.OHLC_VARIABLES[1].OHLC_COUNT == 50);
    CHECK(config.OHLC_VARIABLES[1].OHLC_MINUTES == 60);

    REQUIRE(config.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES.has_value());
    CHECK(config.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES->BUFFER_PIPS == 5);
}

// End-to-end contract between the builder, the mapper and the strategy's
// fail-fast ctor: every combination the sweep queues must produce a config an
// OhlcBreakoutStrategy accepts (names line up, OHLC_COUNT >= 2,
// OHLC_MINUTES >= 1, BUFFER_PIPS present) — otherwise the worker throws at
// construction instead of the grid being caught here at test time. The grid is
// too large to construct literally every combination, but the ctor's checks
// are all per-field, so sweeping each parameter's axis exercises every
// distinct value a field can take; a strided sample of full combinations
// guards the mapper against cross-field surprises.
TEST_CASE("makeOhlcBreakoutStrategy accepts every swept parameter value", "[sweep]") {
    const auto generator = sweep::buildOhlcBreakoutStrategySweep();
    const auto counts = generator.rangeValueCounts();
    const auto total = generator.combinationCount();
    REQUIRE(total > 0);
    REQUIRE(!counts.empty());

    const auto strides = axisStrides(counts);
    for (std::size_t k = 0; k < counts.size(); ++k) {
        for (std::size_t j = 0; j < counts[k].second; ++j) {
            const auto combo = generator.combinationAt(j * strides[k]);
            INFO(counts[k].first << " = " << combo.get(counts[k].first)
                                 << " produced a config the ctor rejects");
            const auto config = sweep::makeOhlcBreakoutStrategy(combo);
            CHECK_NOTHROW(OhlcBreakoutStrategy{config});
        }
    }

    constexpr std::size_t kSamples = 500;
    const std::size_t sampleStride = total > kSamples ? total / kSamples : 1;
    for (std::size_t i = 0; i < total; i += sampleStride) {
        const auto config =
            sweep::makeOhlcBreakoutStrategy(generator.combinationAt(i));
        CHECK_NOTHROW(OhlcBreakoutStrategy{config});
    }
}
