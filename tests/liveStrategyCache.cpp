// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// StrategyCache::build gatekeeping: a winner only becomes a live worker when
// its symbol is tradeable (known to symbol_scale — the tick decoder drops
// unknown symbols, so an unvalidated worker would look cached but never
// receive a tick) and its config carries the trade-lock UUID. One bad winner
// must not affect the rest.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "shared/tradingDefinitions/strategyConfig.hpp"

import liveStrategyCache;
import liveWinners;

namespace {

live::Winner makeWinner(const std::string& symbol, const std::string& uuid) {
    live::Winner winner;
    winner.symbol = symbol;
    winner.strategyName = "RandomStrategy";
    winner.runId = "run-" + uuid;
    winner.performanceScore = 20.0;
    winner.config.UUID = uuid;
    winner.config.TRADING_VARIABLES.STRATEGY = "RandomStrategy";
    winner.config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 25;
    winner.config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 50;
    winner.config.TRADING_VARIABLES.TRADING_SIZE = 3;
    return winner;
}

}  // namespace

TEST_CASE("StrategyCache skips winners whose symbol is not in symbol_scale",
          "[liveStrategyCache]") {
    std::vector<live::Winner> winners;
    winners.push_back(makeWinner("EURUSD", "u-known"));
    winners.push_back(makeWinner("DOGEUSD", "u-unknown"));

    const auto specs = live::StrategyCache::build(winners);

    REQUIRE(specs.size() == 1);
    CHECK(specs[0].symbol == "EURUSD");
    CHECK(specs[0].strategyUuid == "u-known");
}

TEST_CASE("StrategyCache skips winners without a trade-lock UUID",
          "[liveStrategyCache]") {
    std::vector<live::Winner> winners;
    winners.push_back(makeWinner("EURUSD", ""));
    winners.push_back(makeWinner("USDJPY", "u-ok"));

    const auto specs = live::StrategyCache::build(winners);

    REQUIRE(specs.size() == 1);
    CHECK(specs[0].symbol == "USDJPY");
}

TEST_CASE("StrategyCache carries the winner's trading variables into the spec",
          "[liveStrategyCache]") {
    live::Winner winner = makeWinner("EURUSD", "u-vars");
    winner.peakHoursOnly = true;
    const auto specs = live::StrategyCache::build({std::move(winner)});

    REQUIRE(specs.size() == 1);
    CHECK(specs[0].strategyName == "RandomStrategy");
    CHECK(specs[0].vars.STOP_DISTANCE_IN_ATR == 25);
    CHECK(specs[0].vars.LIMIT_DISTANCE_IN_ATR == 50);
    CHECK(specs[0].vars.TRADING_SIZE == 3);
    CHECK(specs[0].peakHoursOnly == true);
    CHECK(specs[0].strategy != nullptr);
}

TEST_CASE("StrategyCache turns RANGE_VARIABLES into worker range series, "
          "skipping the all-zeros sentinel",
          "[liveStrategyCache]") {
    live::Winner ranged = makeWinner("EURUSD", "u-range");
    ranged.config.RANGE_VARIABLES = {
        {.RANGE_ATR_TICK_WINDOW = 5000, .RANGE_ATR_PERCENT = 40,
         .RANGE_COUNT = 50},
        {},  // the all-zeros "unused" sentinel — registers nothing
    };
    // A pre-range winner parses to an empty RANGE_VARIABLES and must build a
    // spec with no range series, exactly as before the field existed.
    live::Winner plain = makeWinner("USDJPY", "u-plain");

    std::vector<live::Winner> winners;
    winners.push_back(std::move(ranged));
    winners.push_back(std::move(plain));
    const auto specs = live::StrategyCache::build(winners);

    REQUIRE(specs.size() == 2);
    REQUIRE(specs[0].rangeSeries.size() == 1);
    CHECK(specs[0].rangeSeries[0].atrTickWindow == 5000);
    CHECK(specs[0].rangeSeries[0].atrPercent == 40);
    CHECK(specs[0].rangeSeries[0].count == 50);
    CHECK(specs[1].rangeSeries.empty());
}
