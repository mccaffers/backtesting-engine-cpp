#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/strategyConfig.hpp"

import ohlcBreakoutStrategy;
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// Breakout timeframe: 3 one-minute candles; trend timeframe: 4 one-minute
// candles (EMA period = 4/2 = 2). Ticks in these tests are spaced 2 minutes
// apart so every tick rolls a fresh bar — warm-up completes after 4 ticks and
// tick 5 is the first that can signal.
tradingDefinitions::StrategyConfig makeConfig(int bufferPips) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-ohlc-breakout";
    config.TRADING_VARIABLES.STRATEGY = "OhlcBreakoutStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_PIPS = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_PIPS = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = 3, .OHLC_MINUTES = 1},  // breakout
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = 4, .OHLC_MINUTES = 1},  // trend
    };
    config.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES =
        tradingDefinitions::OHLCBreakoutVariables{.BUFFER_PIPS = bufferPips};
    return config;
}

PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 std::int32_t bid, const std::string& symbol = "EURUSD") {
    return PriceData(ask, bid, t0 + offset, symbol);
}

// One tick in run-loop order: decide first, then during (runTicks calls
// decide before the management hook on each tick).
std::optional<Direction> step(OhlcBreakoutStrategy& strategy, TradeManager& tm,
                              const PriceData& tick) {
    const auto signal = strategy.decide(tick);
    strategy.during(tick, tm);
    return signal;
}

// Feeds the four warm-up ticks (each starts its own bar) and asserts none of
// them may signal. Bars are built from the ask; bid is ask minus a 2-point
// spread and irrelevant until the signal tick.
void warmUp(OhlcBreakoutStrategy& strategy, TradeManager& tm,
            const std::vector<std::int32_t>& asks,
            const std::string& symbol = "EURUSD") {
    for (std::size_t i = 0; i < asks.size(); ++i) {
        const auto tick = tickAt(minutes{2 * static_cast<int>(i)}, asks[i], asks[i] - 2, symbol);
        CHECK_FALSE(step(strategy, tm, tick).has_value());
    }
}

// Rising EURUSD asks: bars close at 110010/110020/110030, closed breakout
// highs top out at 110020, and the trend EMA sits below the last close
// (macro uptrend).
const std::vector<std::int32_t> kRisingAsks{110000, 110010, 110020, 110030};

// Falling series: closed breakout candles span 110060..110080 and the trend
// EMA sits above the last close (macro downtrend).
const std::vector<std::int32_t> kFallingAsks{110100, 110080, 110060, 110040};

}  // namespace

TEST_CASE("OhlcBreakoutStrategy signals LONG on a buffered breakout in an uptrend",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};  // buffer = 2 pips = 20 points
    TradeManager tm;
    warmUp(strategy, tm, kRisingAsks);

    // Closed-candle highest high is 110020, buffer 20 -> level 110040.
    const auto signal = step(strategy, tm, tickAt(minutes{8}, 110100, 110090));
    CHECK(signal == Direction::LONG);
}

TEST_CASE("OhlcBreakoutStrategy respects the pip buffer", "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    warmUp(strategy, tm, kRisingAsks);

    SECTION("above the high but inside the buffer: no signal") {
        // bid 110035 clears the 110020 high but not the 110040 buffered level.
        CHECK_FALSE(step(strategy, tm, tickAt(minutes{8}, 110045, 110035)).has_value());
    }

    SECTION("exactly on the buffered level: no signal (strictly greater)") {
        CHECK_FALSE(step(strategy, tm, tickAt(minutes{8}, 110050, 110040)).has_value());
    }

    SECTION("one point beyond the buffered level: LONG") {
        CHECK(step(strategy, tm, tickAt(minutes{8}, 110051, 110041)) == Direction::LONG);
    }
}

TEST_CASE("OhlcBreakoutStrategy trend filter blocks counter-trend breakouts",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    warmUp(strategy, tm, kFallingAsks);

    // bid 110200 is far above the closed-candle high (110080) + buffer, but
    // the macro trend is down (last close 110040 < EMA ~110043) — no LONG.
    CHECK_FALSE(step(strategy, tm, tickAt(minutes{8}, 110210, 110200)).has_value());
}

TEST_CASE("OhlcBreakoutStrategy signals SHORT on a buffered breakdown in a downtrend",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    warmUp(strategy, tm, kFallingAsks);

    // Closed-candle lowest low is 110060, buffer 20 -> level 110040.
    SECTION("ask below the buffered low: SHORT") {
        CHECK(step(strategy, tm, tickAt(minutes{8}, 110030, 110020)) == Direction::SHORT);
    }

    SECTION("exactly on the buffered level: no signal (strictly less)") {
        CHECK_FALSE(step(strategy, tm, tickAt(minutes{8}, 110040, 110030)).has_value());
    }
}

TEST_CASE("OhlcBreakoutStrategy keeps per-symbol state isolated", "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;

    // Interleave a rising EURUSD with a falling AUDUSD at a very different
    // price level. If either symbol's ticks leaked into the other's bars, the
    // ranges and trend filters below would be wildly wrong.
    const std::vector<std::int32_t> audAsks{65100, 65080, 65060, 65040};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto offset = minutes{2 * static_cast<int>(i)};
        CHECK_FALSE(step(strategy, tm,
                         tickAt(offset, kRisingAsks[i], kRisingAsks[i] - 2)).has_value());
        CHECK_FALSE(step(strategy, tm,
                         tickAt(offset + seconds{30}, audAsks[i], audAsks[i] - 2, "AUDUSD"))
                        .has_value());
    }

    CHECK(step(strategy, tm, tickAt(minutes{8}, 110100, 110090)) == Direction::LONG);
    CHECK(step(strategy, tm, tickAt(minutes{8} + seconds{30}, 65030, 65020, "AUDUSD")) ==
          Direction::SHORT);
}

TEST_CASE("OhlcBreakoutStrategy rejects malformed configuration", "[ohlcBreakout]") {
    SECTION("fewer than two OHLC timeframes") {
        auto config = makeConfig(2);
        config.OHLC_VARIABLES.resize(1);
        CHECK_THROWS_AS(OhlcBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("missing OHLC_BREAKOUT_VARIABLES") {
        auto config = makeConfig(2);
        config.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(OhlcBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_COUNT below 2") {
        auto config = makeConfig(2);
        config.OHLC_VARIABLES[0].OHLC_COUNT = 1;
        CHECK_THROWS_AS(OhlcBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig(2);
        config.OHLC_VARIABLES[1].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(OhlcBreakoutStrategy{config}, std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips OHLC_BREAKOUT_VARIABLES through JSON",
          "[ohlcBreakout]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.OHLC_BREAKOUT_VARIABLES =
            tradingDefinitions::OHLCBreakoutVariables{.BUFFER_PIPS = 7};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.OHLC_BREAKOUT_VARIABLES.has_value());
        CHECK(back.OHLC_BREAKOUT_VARIABLES->BUFFER_PIPS == 7);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("OHLC_BREAKOUT_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .OHLC_BREAKOUT_VARIABLES.has_value());
    }
}
