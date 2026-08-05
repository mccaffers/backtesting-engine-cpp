#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the bar store off QuestDB
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/strategyConfig.hpp"

import nyOpenRangeBreakoutStrategy;
import barStore;  // bars::BarStore — the strategy reads bars from it
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

// Two UTC midnights, one per DST regime: 2026-01-05 is EST (NY opens 14:30
// UTC), 2026-06-01 is EDT (NY opens 13:30 UTC). Both are Mondays, though the
// strategy itself never reads the weekday — that gate lives in the run loop
// (market_hours::tradePermitted).
const std::chrono::system_clock::time_point kWinterMidnight =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5};
const std::chrono::system_clock::time_point kSummerMidnight =
    std::chrono::sys_days{std::chrono::year{2026} / 6 / 1};

// Signal timeframe: 15m bars, window derived exactly like the sweep mapper —
// ceil((RANGE_HOURS x 60 + entry window) / minutes) + 2 bars, the ctor
// minimum.
tradingDefinitions::StrategyConfig makeConfig(int rangeHours = 4,
                                              int bufferPips = 0,
                                              int entryWindowMinutes = 120,
                                              int maxTradeDurationMinutes = 0,
                                              int ohlcMinutes = 15) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-ny-open-range";
    config.TRADING_VARIABLES.STRATEGY = "NyOpenRangeBreakoutStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    const int ohlcCount =
        (rangeHours * 60 + entryWindowMinutes + ohlcMinutes - 1) / ohlcMinutes +
        2;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = ohlcCount,
                                          .OHLC_MINUTES = ohlcMinutes},
    };
    config.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES =
        tradingDefinitions::NyOpenRangeBreakoutVariables{
            .RANGE_HOURS = rangeHours,
            .BUFFER_PIPS = bufferPips,
            .ENTRY_WINDOW_MINUTES = entryWindowMinutes,
            .MAX_TRADE_DURATION_MINUTES = maxTradeDurationMinutes};
    return config;
}

PriceData tickAt(std::chrono::system_clock::time_point base,
                 std::chrono::system_clock::duration offset, std::int32_t ask,
                 std::int32_t bid, const std::string& symbol = "EURUSD") {
    return PriceData(ask, bid, base + offset, symbol);
}

// The loop owner's role in miniature: register every configured timeframe.
bars::BarStore makeStore(const tradingDefinitions::StrategyConfig& config) {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query
    bars::BarStore store;
    for (const auto& ohlc : config.OHLC_VARIABLES) {
        store.registerSeries(minutes{ohlc.OHLC_MINUTES}, ohlc.OHLC_COUNT);
    }
    return store;
}

// One tick in run-loop order: the store update first, then decide, then the
// management hook — runTicks feeds the shared bars BEFORE the entry gates,
// so decide() judges the tick against bar state that already includes it.
std::optional<Direction> step(NyOpenRangeBreakoutStrategy& strategy,
                              TradeManager& tm, bars::BarStore& store,
                              const PriceData& tick) {
    store.update(tick);
    const auto signal = strategy.decide(tick, store);
    strategy.during(tick, store, tm);
    return signal;
}

// The exact OHLC a crafted 15m bar should end up with.
struct BarShape {
    std::int32_t o, h, l, c;
};

// Four asks inside one 15m bar, fed open/high/low/close within 45 seconds of
// `offset`. Bars are FIRST-TICK anchored (ohlcBuilder rolls on the first tick
// strictly more than one duration past the bar's start), so consecutive
// feeds must sit more than 15 minutes apart to land in separate bars. Every
// feed tick is pre-open under BOTH DST regimes (all before 13:30 UTC), so
// the clock gate guarantees no signal.
void feedBar(NyOpenRangeBreakoutStrategy& strategy, TradeManager& tm,
             bars::BarStore& store, std::chrono::system_clock::time_point base,
             minutes offset, const BarShape& bar,
             const std::string& symbol = "EURUSD") {
    const std::array<std::pair<seconds, std::int32_t>, 4> ticks{{
        {seconds{0}, bar.o},
        {seconds{15}, bar.h},
        {seconds{30}, bar.l},
        {seconds{45}, bar.c},
    }};
    for (const auto& [tickOffset, ask] : ticks) {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(base, offset + tickOffset, ask, ask - 10, symbol))
                        .has_value());
    }
}

// Canonical pre-open fixture around `base` (a UTC midnight), built for
// RANGE_HOURS = 4 in BOTH DST regimes: the range window is [10:30, 14:30)
// UTC in winter and [09:30, 13:30) under EDT, so every range bar below sits
// inside both and the fixture's tradeable extremes are identical either way.
//   09:20  pre-range bar — satisfies the coverage gate in both regimes (its
//          high is the `preRangeHigh` knob so a test can plant an extreme
//          there; it must stay OUT of the range)
//   10:40/11:40/12:20 — the pre-open range: high 110050, low 109950
//   13:00 — the last range bar; the decision tick itself rolls it closed
void feedPreOpenFixture(NyOpenRangeBreakoutStrategy& strategy,
                        TradeManager& tm, bars::BarStore& store,
                        std::chrono::system_clock::time_point base,
                        bool withCoverageBar = true,
                        std::int32_t preRangeHigh = 110005) {
    if (withCoverageBar) {
        feedBar(strategy, tm, store, base, minutes{560},
                {110000, preRangeHigh, 109995, 110000});
    }
    // Without the coverage bar the series' oldest bar starts 10:40, strictly
    // after the winter range start (10:30) — partial coverage.
    feedBar(strategy, tm, store, base, minutes{640},
            {110000, 110020, 109980, 110010});
    feedBar(strategy, tm, store, base, minutes{700},
            {110010, 110050, 109990, 110030});  // range high 110050
    feedBar(strategy, tm, store, base, minutes{740},
            {110030, 110040, 109950, 109990});  // range low 109950
    feedBar(strategy, tm, store, base, minutes{780},
            {109990, 110030, 109970, 110000});  // 13:00 — last range bar
}

}  // namespace

TEST_CASE("NyOpenRangeBreakoutStrategy trades the NY break of the pre-open range",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedPreOpenFixture(strategy, tm, store, kWinterMidnight);

    // Winter: NY opens 14:30 UTC; the window (120m) runs to 16:30.
    SECTION("bid above the range high inside the window: LONG") {
        CHECK(step(strategy, tm, store,
                   tickAt(kWinterMidnight, minutes{875}, 110071, 110061)) ==
              Direction::LONG);
    }

    SECTION("bid exactly on the range high: no signal (strictly above)") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{875}, 110060, 110050))
                        .has_value());
    }

    SECTION("ask below the range low inside the window: SHORT") {
        CHECK(step(strategy, tm, store,
                   tickAt(kWinterMidnight, minutes{875}, 109940, 109930)) ==
              Direction::SHORT);
    }
}

TEST_CASE("NyOpenRangeBreakoutStrategy only fires inside the entry window",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedPreOpenFixture(strategy, tm, store, kWinterMidnight);

    SECTION("a breakout before the winter open (13:35) is refused") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{815}, 110071, 110061))
                        .has_value());
    }

    SECTION("a breakout after the window closes (16:35) is refused") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{995}, 110071, 110061))
                        .has_value());
    }
}

TEST_CASE("NyOpenRangeBreakoutStrategy follows the EDT NY open",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedPreOpenFixture(strategy, tm, store, kSummerMidnight);

    // 13:35 is pre-open in winter (see above) but inside the window under
    // EDT — the same offset flipping proves the DST rule is consulted.
    CHECK(step(strategy, tm, store,
               tickAt(kSummerMidnight, minutes{815}, 110071, 110061)) ==
          Direction::LONG);
}

TEST_CASE("NyOpenRangeBreakoutStrategy pads the range with BUFFER_PIPS",
          "[nyOpenRangeBreakout]") {
    // 2 pips = 20 points on EURUSD: the padded high sits at 110070.
    NyOpenRangeBreakoutStrategy strategy{makeConfig(4, 2)};
    TradeManager tm;
    auto store = makeStore(makeConfig(4, 2));
    feedPreOpenFixture(strategy, tm, store, kWinterMidnight);

    SECTION("a poke through the raw high but not the padding: no signal") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{875}, 110071, 110061))
                        .has_value());
    }

    SECTION("clearing the padded high: LONG") {
        CHECK(step(strategy, tm, store,
                   tickAt(kWinterMidnight, minutes{875}, 110081, 110071)) ==
              Direction::LONG);
    }
}

TEST_CASE("NyOpenRangeBreakoutStrategy keeps pre-range bars out of the range",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    // The 09:20 bar spikes to 110200 — it starts before the range window, so
    // the tradeable high must stay 110050.
    feedPreOpenFixture(strategy, tm, store, kWinterMidnight, true, 110200);

    CHECK(step(strategy, tm, store,
               tickAt(kWinterMidnight, minutes{875}, 110071, 110061)) ==
          Direction::LONG);
}

TEST_CASE("NyOpenRangeBreakoutStrategy refuses a partially covered range",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    // No coverage bar: the series' oldest bar starts 10:40, strictly after
    // the winter range start (10:30), so the pre-open window is only
    // partially represented — a fragment range (a run's first day) must not
    // trade.
    feedPreOpenFixture(strategy, tm, store, kWinterMidnight, false);

    CHECK_FALSE(step(strategy, tm, store,
                     tickAt(kWinterMidnight, minutes{875}, 110071, 110061))
                    .has_value());
}

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — same harness as the SessionRangeBreakoutStrategy cap tests.
TEST_CASE("NyOpenRangeBreakoutStrategy closes trades past the max duration via during()",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig(4, 0, 120, 60)};
    TradeManager tm;

    SECTION("LONG past the cap closes at the bid") {
        tm.openTrade(tickAt(kWinterMidnight, seconds{0}, 110000, 109998), 1,
                     Direction::LONG);
        strategy.during(
            tickAt(kWinterMidnight, minutes{60} + seconds{1}, 110050, 110040), bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110040);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("SHORT past the cap closes at the ask") {
        tm.openTrade(tickAt(kWinterMidnight, seconds{0}, 110000, 109998), 1,
                     Direction::SHORT);
        strategy.during(
            tickAt(kWinterMidnight, minutes{60} + seconds{1}, 110050, 110040), bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110050);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("exactly at the cap stays open (strictly greater)") {
        tm.openTrade(tickAt(kWinterMidnight, seconds{0}, 110000, 109998), 1,
                     Direction::LONG);
        strategy.during(tickAt(kWinterMidnight, minutes{60}, 110050, 110040), bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
}

TEST_CASE("NyOpenRangeBreakoutStrategy max duration of zero disables the exit",
          "[nyOpenRangeBreakout]") {
    NyOpenRangeBreakoutStrategy strategy{makeConfig(4, 0, 120, 0)};
    TradeManager tm;

    tm.openTrade(tickAt(kWinterMidnight, seconds{0}, 110000, 109998), 1,
                 Direction::LONG);
    strategy.during(tickAt(kWinterMidnight, minutes{600}, 110050, 110040), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("NyOpenRangeBreakoutStrategy rejects malformed configuration",
          "[nyOpenRangeBreakout]") {
    SECTION("no OHLC timeframe") {
        auto config = makeConfig();
        config.OHLC_VARIABLES.clear();
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("missing NY_OPEN_RANGE_BREAKOUT_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("RANGE_HOURS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES->RANGE_HOURS = 0;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("RANGE_HOURS reaching past the previous UTC midnight") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES->RANGE_HOURS = 14;
        // Keep the window check satisfied so the depth cap is what throws.
        config.OHLC_VARIABLES[0].OHLC_COUNT = 200;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("ENTRY_WINDOW_MINUTES below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES
            ->ENTRY_WINDOW_MINUTES = 0;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("negative BUFFER_PIPS") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES->BUFFER_PIPS = -1;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("window one bar short of range start -> entry cutoff coverage") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(NyOpenRangeBreakoutStrategy{config}, std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips NY_OPEN_RANGE_BREAKOUT_VARIABLES through JSON",
          "[nyOpenRangeBreakout]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.NY_OPEN_RANGE_BREAKOUT_VARIABLES =
            tradingDefinitions::NyOpenRangeBreakoutVariables{
                .RANGE_HOURS = 8,
                .BUFFER_PIPS = 3,
                .ENTRY_WINDOW_MINUTES = 90,
                .MAX_TRADE_DURATION_MINUTES = 45};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.NY_OPEN_RANGE_BREAKOUT_VARIABLES.has_value());
        CHECK(back.NY_OPEN_RANGE_BREAKOUT_VARIABLES->RANGE_HOURS == 8);
        CHECK(back.NY_OPEN_RANGE_BREAKOUT_VARIABLES->BUFFER_PIPS == 3);
        CHECK(back.NY_OPEN_RANGE_BREAKOUT_VARIABLES->ENTRY_WINDOW_MINUTES == 90);
        CHECK(back.NY_OPEN_RANGE_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 45);
    }

    SECTION("absent MAX_TRADE_DURATION_MINUTES parses as disabled") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0, not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"NY_OPEN_RANGE_BREAKOUT_VARIABLES":{"RANGE_HOURS":4,)"
                R"("BUFFER_PIPS":2,"ENTRY_WINDOW_MINUTES":60}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.NY_OPEN_RANGE_BREAKOUT_VARIABLES.has_value());
        CHECK(vars.NY_OPEN_RANGE_BREAKOUT_VARIABLES->ENTRY_WINDOW_MINUTES == 60);
        CHECK(vars.NY_OPEN_RANGE_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("NY_OPEN_RANGE_BREAKOUT_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .NY_OPEN_RANGE_BREAKOUT_VARIABLES.has_value());
    }
}
