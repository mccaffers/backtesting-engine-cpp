#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the bar store off QuestDB
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/strategyConfig.hpp"

import ohlcBreakoutStrategy;
import barStore;  // bars::BarStore — the strategy reads bars from it
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// Breakout timeframe: 3 one-minute candles; trend timeframe: 4 candles of
// `trendMinutes` (EMA period = 4/2 = 2). Ticks in these tests are spaced 2
// minutes apart so every tick rolls a fresh 1m bar. The store updates BEFORE
// decide(), so the 4th tick is already warm — the fixtures are chosen so
// none of the warm-up ticks signals.
tradingDefinitions::StrategyConfig makeConfig(int bufferPips,
                                              int maxTradeDurationMinutes = 0,
                                              int trendMinutes = 1) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-ohlc-breakout";
    config.TRADING_VARIABLES.STRATEGY = "OhlcBreakoutStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = 3, .OHLC_MINUTES = 1},  // breakout
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = 4,
                                          .OHLC_MINUTES = trendMinutes},  // trend
    };
    config.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES =
        tradingDefinitions::OHLCBreakoutVariables{
            .BUFFER_PIPS = bufferPips,
            .MAX_TRADE_DURATION_MINUTES = maxTradeDurationMinutes};
    return config;
}

PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 std::int32_t bid, const std::string& symbol = "EURUSD") {
    return PriceData(ask, bid, t0 + offset, symbol);
}

// The loop owner's role in miniature: one store carrying both of the config's
// timeframes. When both are 1 minute they dedup into one shared series with
// the larger window (4); each consumer reads its own tail, like production.
bars::BarStore makeStore(int trendMinutes = 1) {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query
    bars::BarStore store;
    store.registerSeries(minutes{1}, 3);             // breakout timeframe
    store.registerSeries(minutes{trendMinutes}, 4);  // trend timeframe
    return store;
}

// One tick in run-loop order: the store update first, then decide, then the
// management hook — runTicks feeds the shared bars BEFORE the entry gates,
// so decide() judges the tick against bar state that already includes it.
std::optional<Direction> step(OhlcBreakoutStrategy& strategy, TradeManager& tm,
                              bars::BarStore& store, const PriceData& tick) {
    store.update(tick);
    const auto signal = strategy.decide(tick, store);
    strategy.during(tick, store, tm);
    return signal;
}

// Feeds the four warm-up ticks (each starts its own bar) and asserts none of
// them may signal. Bars are built from the ask; bid is ask minus a 2-point
// spread and irrelevant until the signal tick.
void warmUp(OhlcBreakoutStrategy& strategy, TradeManager& tm,
            bars::BarStore& store, const std::vector<std::int32_t>& asks,
            const std::string& symbol = "EURUSD") {
    for (std::size_t i = 0; i < asks.size(); ++i) {
        const auto tick = tickAt(minutes{2 * static_cast<int>(i)}, asks[i], asks[i] - 2, symbol);
        CHECK_FALSE(step(strategy, tm, store, tick).has_value());
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
    auto store = makeStore();
    warmUp(strategy, tm, store, kRisingAsks);

    // The signal tick rolls its own (in-progress) bar first, so the closed
    // breakout candles are 110020/110030: high 110030, buffer 20 -> 110050.
    const auto signal =
        step(strategy, tm, store, tickAt(minutes{8}, 110100, 110090));
    CHECK(signal == Direction::LONG);
}

TEST_CASE("OhlcBreakoutStrategy respects the pip buffer", "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    auto store = makeStore();
    warmUp(strategy, tm, store, kRisingAsks);

    // Closed candles at the judged tick are 110020/110030: high 110030,
    // buffer 20 -> buffered level 110050.
    SECTION("above the high but inside the buffer: no signal") {
        // bid 110035 clears the 110030 high but not the 110050 buffered level.
        CHECK_FALSE(
            step(strategy, tm, store, tickAt(minutes{8}, 110045, 110035)).has_value());
    }

    SECTION("exactly on the buffered level: no signal (strictly greater)") {
        CHECK_FALSE(
            step(strategy, tm, store, tickAt(minutes{8}, 110060, 110050)).has_value());
    }

    SECTION("one point beyond the buffered level: LONG") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110061, 110051)) ==
              Direction::LONG);
    }
}

// The current tick's own close counts as trend evidence now (the store
// updates pre-decide), so blocking a counter-trend breakout requires
// genuinely separated timeframes: a steep multi-hour fall holds the 60m EMA
// far above a small local pop that clears the 1m range.
TEST_CASE("OhlcBreakoutStrategy trend filter blocks counter-trend breakouts",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2, 0, 60)};  // trend = 60m bars
    TradeManager tm;
    auto store = makeStore(60);

    // Four 60m trend bars stepping down 1000 points (each tick also rolls a
    // 1m breakout bar). Signals during the descent are not under test.
    step(strategy, tm, store, tickAt(minutes{0}, 110000, 109998));
    step(strategy, tm, store, tickAt(minutes{61}, 109000, 108998));
    step(strategy, tm, store, tickAt(minutes{122}, 108000, 107998));
    step(strategy, tm, store, tickAt(minutes{183}, 107000, 106998));

    // A tight local range at the bottom, inside the 4th 60m bar.
    step(strategy, tm, store, tickAt(minutes{185}, 107010, 107008));
    step(strategy, tm, store, tickAt(minutes{187}, 107020, 107018));

    // bid 107190 clears the local closed-candle high (107020) + buffer by a
    // mile, but the 60m EMA (~107633) is still far overhead — no LONG (and
    // nowhere near the local low, so no SHORT either).
    CHECK_FALSE(
        step(strategy, tm, store, tickAt(minutes{189}, 107200, 107190)).has_value());
}

TEST_CASE("OhlcBreakoutStrategy signals SHORT on a buffered breakdown in a downtrend",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    auto store = makeStore();
    warmUp(strategy, tm, store, kFallingAsks);

    // Closed candles at the judged tick are 110060/110040: lowest low
    // 110040, buffer 20 -> buffered level 110020.
    SECTION("ask below the buffered low: SHORT") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110019, 110009)) ==
              Direction::SHORT);
    }

    SECTION("exactly on the buffered level: no signal (strictly less)") {
        CHECK_FALSE(
            step(strategy, tm, store, tickAt(minutes{8}, 110020, 110010)).has_value());
    }
}

TEST_CASE("OhlcBreakoutStrategy keeps per-symbol state isolated", "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    auto store = makeStore();

    // Interleave a rising EURUSD with a falling AUDUSD at a very different
    // price level. If either symbol's ticks leaked into the other's bars, the
    // ranges and trend filters below would be wildly wrong.
    const std::vector<std::int32_t> audAsks{65100, 65080, 65060, 65040};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto offset = minutes{2 * static_cast<int>(i)};
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(offset, kRisingAsks[i], kRisingAsks[i] - 2)).has_value());
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(offset + seconds{30}, audAsks[i], audAsks[i] - 2, "AUDUSD"))
                        .has_value());
    }

    CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110100, 110090)) ==
          Direction::LONG);
    // AUDUSD closed candles at the judged tick are 65060/65040: low 65040,
    // buffer 20 -> level 65020, so the ask must undercut 65020.
    CHECK(step(strategy, tm, store,
               tickAt(minutes{8} + seconds{30}, 65010, 65000, "AUDUSD")) ==
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

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — exactly how the live book seeds them (openTrade from a
// synthetic tick stamped with the broker's openedAt).
TEST_CASE("OhlcBreakoutStrategy closes trades past the max duration via during()",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(0, 60)};
    TradeManager tm;

    SECTION("LONG past the cap closes at the bid") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050, 110040), bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110040);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("SHORT past the cap closes at the ask") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::SHORT);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050, 110040), bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110050);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("exactly at the cap stays open (strictly greater)") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60}, 110050, 110040), bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }

    SECTION("another symbol's tick never closes it") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::LONG);
        strategy.during(tickAt(minutes{120}, 65030, 65020, "AUDUSD"), bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
}

TEST_CASE("OhlcBreakoutStrategy max duration of zero disables the exit",
          "[ohlcBreakout]") {
    OhlcBreakoutStrategy strategy{makeConfig(0, 0)};
    TradeManager tm;

    tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::LONG);
    strategy.during(tickAt(minutes{600}, 110050, 110040), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("StrategyVariables round-trips OHLC_BREAKOUT_VARIABLES through JSON",
          "[ohlcBreakout]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.OHLC_BREAKOUT_VARIABLES = tradingDefinitions::OHLCBreakoutVariables{
            .BUFFER_PIPS = 7, .MAX_TRADE_DURATION_MINUTES = 45};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.OHLC_BREAKOUT_VARIABLES.has_value());
        CHECK(back.OHLC_BREAKOUT_VARIABLES->BUFFER_PIPS == 7);
        CHECK(back.OHLC_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 45);
    }

    SECTION("absent MAX_TRADE_DURATION_MINUTES parses as disabled") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0 (disabled), not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"OHLC_RSI_VARIABLES":null,)"
                R"("OHLC_BREAKOUT_VARIABLES":{"BUFFER_PIPS":7}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.OHLC_BREAKOUT_VARIABLES.has_value());
        CHECK(vars.OHLC_BREAKOUT_VARIABLES->BUFFER_PIPS == 7);
        CHECK(vars.OHLC_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("OHLC_BREAKOUT_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .OHLC_BREAKOUT_VARIABLES.has_value());
    }
}
