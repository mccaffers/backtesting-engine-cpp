// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#import <XCTest/XCTest.h>
#import <optional>
#import <set>
#import <string>
#import <utility>
#import <vector>
#import <boost/decimal/literals.hpp>
#import <nlohmann/json.hpp>
#import "parameterSweep.hpp"
#import "randomStrategySweep.hpp"
#import "runConfigurationBuilder.hpp"
#import "tradeManager.hpp"
#import "run_configuration.hpp"
#import "strategies/randomStrategy.hpp"
#import "trading_definitions/strategy.hpp"

// Pulls in the _dd user-defined literal so "5"_dd produces a decimal64_t
// directly — same convention as tradeManager.mm.
using namespace boost::decimal::literals;

@interface SweepTests : XCTestCase
@end

@implementation SweepTests

#pragma mark - buildRandomStrategySweep

- (void)testBuildRandomStrategySweep_RegistersTwoParameters {
    const auto generator = sweep::buildRandomStrategySweep();
    XCTAssertEqual(generator.parameterCount(), 2,
                   "RandomStrategy sweeps exactly STOP and LIMIT pip distances");
}

- (void)testBuildRandomStrategySweep_GeneratesFullCartesianProduct {
    const auto combinations =
        sweep::buildRandomStrategySweep().generateAllCombinations();
    XCTAssertEqual(combinations.size(), 9,
                   "3 stop values x 3 limit values must expand to 9 combinations");
    for (const auto& combo : combinations) {
        XCTAssertTrue(combo.has("STOP_DISTANCE_IN_PIPS"),
                      "Every combination must carry the stop distance");
        XCTAssertTrue(combo.has("LIMIT_DISTANCE_IN_PIPS"),
                      "Every combination must carry the limit distance");
        // The OHLC ranges are commented out in the builder; loadCommand's
        // makeStrategy relies on has() returning false so the fields default
        // to 0 instead of throwing in get().
        XCTAssertFalse(combo.has("OHLC_COUNT"),
                       "OHLC_COUNT is not currently swept");
        XCTAssertFalse(combo.has("OHLC_MINUTES"),
                       "OHLC_MINUTES is not currently swept");
    }
}

- (void)testBuildRandomStrategySweep_CoversEveryStopLimitPair {
    const auto combinations =
        sweep::buildRandomStrategySweep().generateAllCombinations();

    std::set<std::pair<double, double>> pairs;
    for (const auto& combo : combinations) {
        pairs.emplace(combo.get("STOP_DISTANCE_IN_PIPS"),
                      combo.get("LIMIT_DISTANCE_IN_PIPS"));
    }

    XCTAssertEqual(pairs.size(), 9, "All 9 combinations must be distinct");
    for (const double stop : {1.0, 1.5, 10.0}) {
        for (const double limit : {1.0, 1.5, 10.0}) {
            XCTAssertTrue(pairs.count({stop, limit}) == 1,
                          "Missing stop=%.1f / limit=%.1f from the grid", stop, limit);
        }
    }
}

// generateAllCombinations expands ranges in registration order with the
// last-registered parameter varying fastest. RUN output ordering feeds the
// queue, so pin it: the first three combinations hold STOP at 1.0 while
// LIMIT walks the list.
- (void)testBuildRandomStrategySweep_OrderIsDeterministic {
    const auto combinations =
        sweep::buildRandomStrategySweep().generateAllCombinations();
    XCTAssertEqual(combinations.size(), 9, "Pre-condition: full grid");

    const double stops[] = {1.0, 1.0, 1.0, 1.5, 1.5, 1.5, 10.0, 10.0, 10.0};
    const double limits[] = {1.0, 1.5, 10.0, 1.0, 1.5, 10.0, 1.0, 1.5, 10.0};
    for (std::size_t i = 0; i < combinations.size(); ++i) {
        XCTAssertEqual(combinations[i].get("STOP_DISTANCE_IN_PIPS"), stops[i],
                       "STOP order must be stable (slow axis)");
        XCTAssertEqual(combinations[i].get("LIMIT_DISTANCE_IN_PIPS"), limits[i],
                       "LIMIT order must be stable (fast axis)");
    }
}

#pragma mark - makeRunConfiguration

- (void)testMakeRunConfiguration_CarriesRunId {
    const auto config = sweep::makeRunConfiguration("test-run-id");
    XCTAssertEqual(config.RUN_ID, std::string("test-run-id"),
                   "RUN_ID links the run descriptor to its swept strategies");
}

- (void)testMakeRunConfiguration_RunDescriptorValues {
    const auto config = sweep::makeRunConfiguration("test-run-id");
    XCTAssertEqual(config.SYMBOLS, std::string("EURUSD"), "Tick data symbol");
    XCTAssertEqual(config.LAST_MONTHS, 6, "Tick data window in months");
    XCTAssertEqual(config.STARTING_BALANCE, trading_definitions::DEFAULT_STARTING_BALANCE,
                   "Runs start from the shared default balance");
    XCTAssertEqual(config.MAX_LOSS_PERCENT, "5"_dd,
                   "Runs cut off after losing 5%% of the account");
    XCTAssertEqual(config.MAX_OPEN_TRADES, 10, "Cap on simultaneous open positions");
    XCTAssertTrue(config.REPORT_FAILURES, "Liquidated runs currently still report");
}

// The descriptor travels through Redis as JSON (loadCommand dumps it, the
// runner parses it back), so the round-trip must preserve every field —
// including the decimal ones, which move as strings via decimal_json.hpp.
- (void)testMakeRunConfiguration_SurvivesJsonRoundTrip {
    const auto original = sweep::makeRunConfiguration("round-trip-id");

    const nlohmann::json j = original;
    const auto restored = j.get<trading_definitions::RunConfiguration>();

    XCTAssertEqual(restored.RUN_ID, original.RUN_ID, "RUN_ID must survive");
    XCTAssertEqual(restored.SYMBOLS, original.SYMBOLS, "SYMBOLS must survive");
    XCTAssertEqual(restored.LAST_MONTHS, original.LAST_MONTHS, "LAST_MONTHS must survive");
    XCTAssertEqual(restored.STARTING_BALANCE, original.STARTING_BALANCE,
                   "STARTING_BALANCE must survive exactly (decimal-as-string)");
    XCTAssertEqual(restored.MAX_LOSS_PERCENT, original.MAX_LOSS_PERCENT,
                   "MAX_LOSS_PERCENT must survive exactly (decimal-as-string)");
    XCTAssertEqual(restored.MAX_OPEN_TRADES, original.MAX_OPEN_TRADES,
                   "MAX_OPEN_TRADES must survive");
    XCTAssertEqual(restored.REPORT_FAILURES, original.REPORT_FAILURES,
                   "REPORT_FAILURES must survive");
}

#pragma mark - RandomStrategy (the strategy the sweep drives)

// RandomStrategy ignores everything in its config, so default-constructed
// Strategy is enough — selectStrategy only needs the name, and these tests
// construct the class directly.
- (void)testRandomStrategy_AlwaysReturnsASignal {
    RandomStrategy strategy{trading_definitions::Strategy{}};
    PriceData tick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");

    for (int i = 0; i < 100; ++i) {
        const auto signal = strategy.decide(tick);
        XCTAssertTrue(signal.has_value(),
                      "The coin flip always lands — decide() must never return nullopt");
        XCTAssertTrue(*signal == Direction::LONG || *signal == Direction::SHORT,
                      "Signal must be one of the two directions");
    }
}

// A fair coin over 1000 flips produces both directions with probability
// 1 - 2^-999 — a single-sided run means the distribution (or seeding) broke.
- (void)testRandomStrategy_ProducesBothDirections {
    RandomStrategy strategy{trading_definitions::Strategy{}};
    PriceData tick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");

    bool sawLong = false;
    bool sawShort = false;
    for (int i = 0; i < 1000 && !(sawLong && sawShort); ++i) {
        const auto signal = strategy.decide(tick);
        sawLong = sawLong || (signal == Direction::LONG);
        sawShort = sawShort || (signal == Direction::SHORT);
    }
    XCTAssertTrue(sawLong, "1000 fair flips must produce at least one LONG");
    XCTAssertTrue(sawShort, "1000 fair flips must produce at least one SHORT");
}

// Exits are owned by Operations via SL/TP — during() must not touch open
// positions, otherwise the sweep's stop/limit parameters stop being the only
// exit mechanism under test.
- (void)testRandomStrategy_DuringLeavesOpenTradesAlone {
    RandomStrategy strategy{trading_definitions::Strategy{}};
    TradeManager manager;
    PriceData tick("1.1001"_dd, "1.1000"_dd, std::chrono::system_clock::now(), "EURUSD");
    manager.openTrade(tick, "1.0"_dd, Direction::LONG);

    strategy.during(tick, manager);

    XCTAssertEqual(manager.getActiveTrades().size(), 1,
                   "during() is a no-op — the open trade must remain untouched");
    XCTAssertEqual(manager.getClosedTrades().size(), 0,
                   "during() must not close anything");
}

@end
