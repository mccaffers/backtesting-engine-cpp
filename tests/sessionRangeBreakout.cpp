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

import sessionRangeBreakoutStrategy;
import barStore;  // bars::BarStore — the strategy reads bars from it
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

// Two UTC midnights, one per DST regime: 2026-01-05 is GMT (London opens
// 08:00), 2026-06-01 is BST (London opens 07:00). Both are Mondays, though
// the strategy itself never reads the weekday — that gate lives in the run
// loop (market_hours::tradePermitted).
const std::chrono::system_clock::time_point kWinterMidnight =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5};
const std::chrono::system_clock::time_point kSummerMidnight =
    std::chrono::sys_days{std::chrono::year{2026} / 6 / 1};

// Signal timeframe: 15m bars, window derived exactly like the sweep mapper —
// ceil((480 + entry window) / minutes) + 2 bars, the ctor minimum.
tradingDefinitions::StrategyConfig makeConfig(int bufferPips = 0,
                                              int entryWindowMinutes = 120,
                                              int maxTradeDurationMinutes = 0,
                                              int ohlcMinutes = 15) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-session-range";
    config.TRADING_VARIABLES.STRATEGY = "SessionRangeBreakoutStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    const int ohlcCount =
        (480 + entryWindowMinutes + ohlcMinutes - 1) / ohlcMinutes + 2;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = ohlcCount,
                                          .OHLC_MINUTES = ohlcMinutes},
    };
    config.STRATEGY_VARIABLES.SESSION_RANGE_BREAKOUT_VARIABLES =
        tradingDefinitions::SessionRangeBreakoutVariables{
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
std::optional<Direction> step(SessionRangeBreakoutStrategy& strategy,
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
// feed tick is pre-open, so the clock gate guarantees no signal.
void feedBar(SessionRangeBreakoutStrategy& strategy, TradeManager& tm,
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

// Canonical session fixture around `base` (a UTC midnight):
//   23:40 (prev day)  flat bar — satisfies the pre-midnight coverage gate
//   00:00/02:00/04:00/05:40 — the Asian session: high 110050, low 109950
//   06:30 — a post-Asia bar that must stay OUT of the range (its high is the
//           `postAsiaHigh` knob so a test can plant an extreme there); it
//           also rolls the 05:40 bar closed before the entry window opens.
void feedSessionFixture(SessionRangeBreakoutStrategy& strategy,
                        TradeManager& tm, bars::BarStore& store,
                        std::chrono::system_clock::time_point base,
                        bool withPreMidnightBar = true,
                        std::int32_t postAsiaHigh = 110020) {
    if (withPreMidnightBar) {
        feedBar(strategy, tm, store, base, minutes{-20},
                {110000, 110005, 109995, 110000});
    }
    // Without the pre-midnight bar the first Asian bar starts at 00:20, so
    // the series' oldest bar sits strictly after midnight — partial coverage.
    feedBar(strategy, tm, store, base, withPreMidnightBar ? minutes{0} : minutes{20},
            {110000, 110020, 109980, 110010});
    feedBar(strategy, tm, store, base, minutes{120},
            {110010, 110050, 109990, 110030});  // session high 110050
    feedBar(strategy, tm, store, base, minutes{240},
            {110030, 110040, 109950, 109990});  // session low 109950
    feedBar(strategy, tm, store, base, minutes{340},
            {109990, 110030, 109970, 110000});
    feedBar(strategy, tm, store, base, minutes{390},
            {110000, postAsiaHigh, 109980, 110000});  // 06:30 — not Asia
}

}  // namespace

TEST_CASE("SessionRangeBreakoutStrategy trades the London break of the Asian range",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedSessionFixture(strategy, tm, store, kWinterMidnight);

    // Winter: London opens 08:00 UTC; the window (120m) runs to 10:00.
    SECTION("bid above the Asian high inside the window: LONG") {
        CHECK(step(strategy, tm, store,
                   tickAt(kWinterMidnight, minutes{485}, 110071, 110061)) ==
              Direction::LONG);
    }

    SECTION("bid exactly on the Asian high: no signal (strictly above)") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{485}, 110060, 110050))
                        .has_value());
    }

    SECTION("ask below the Asian low inside the window: SHORT") {
        CHECK(step(strategy, tm, store,
                   tickAt(kWinterMidnight, minutes{485}, 109940, 109930)) ==
              Direction::SHORT);
    }
}

TEST_CASE("SessionRangeBreakoutStrategy only fires inside the entry window",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedSessionFixture(strategy, tm, store, kWinterMidnight);

    SECTION("a breakout before the winter open (07:05) is refused") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{425}, 110071, 110061))
                        .has_value());
    }

    SECTION("a breakout after the window closes (10:05) is refused") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{605}, 110071, 110061))
                        .has_value());
    }
}

TEST_CASE("SessionRangeBreakoutStrategy follows the BST London open",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedSessionFixture(strategy, tm, store, kSummerMidnight);

    // 07:05 is pre-open in winter (see above) but inside the window under
    // BST — the same offset flipping proves the DST rule is consulted.
    CHECK(step(strategy, tm, store,
               tickAt(kSummerMidnight, minutes{425}, 110071, 110061)) ==
          Direction::LONG);
}

TEST_CASE("SessionRangeBreakoutStrategy pads the range with BUFFER_PIPS",
          "[sessionRangeBreakout]") {
    // 2 pips = 20 points on EURUSD: the padded high sits at 110070.
    SessionRangeBreakoutStrategy strategy{makeConfig(2)};
    TradeManager tm;
    auto store = makeStore(makeConfig(2));
    feedSessionFixture(strategy, tm, store, kWinterMidnight);

    SECTION("a poke through the raw high but not the padding: no signal") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kWinterMidnight, minutes{485}, 110071, 110061))
                        .has_value());
    }

    SECTION("clearing the padded high: LONG") {
        CHECK(step(strategy, tm, store,
                   tickAt(kWinterMidnight, minutes{485}, 110081, 110071)) ==
              Direction::LONG);
    }
}

TEST_CASE("SessionRangeBreakoutStrategy keeps post-session bars out of the range",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    // The 06:30 bar spikes to 110200 — outside the Asian window, so the
    // tradeable high must stay 110050.
    feedSessionFixture(strategy, tm, store, kWinterMidnight, true, 110200);

    CHECK(step(strategy, tm, store,
               tickAt(kWinterMidnight, minutes{485}, 110071, 110061)) ==
          Direction::LONG);
}

TEST_CASE("SessionRangeBreakoutStrategy refuses a partially covered session",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    // No pre-midnight bar: the series' oldest bar starts 00:20, so today's
    // Asian session is only partially represented — a fragment range (a
    // run's first day) must not trade.
    feedSessionFixture(strategy, tm, store, kWinterMidnight, false);

    CHECK_FALSE(step(strategy, tm, store,
                     tickAt(kWinterMidnight, minutes{485}, 110071, 110061))
                    .has_value());
}

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — same harness as the OhlcBreakoutStrategy cap tests.
TEST_CASE("SessionRangeBreakoutStrategy closes trades past the max duration via during()",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig(0, 120, 60)};
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

TEST_CASE("SessionRangeBreakoutStrategy max duration of zero disables the exit",
          "[sessionRangeBreakout]") {
    SessionRangeBreakoutStrategy strategy{makeConfig(0, 120, 0)};
    TradeManager tm;

    tm.openTrade(tickAt(kWinterMidnight, seconds{0}, 110000, 109998), 1,
                 Direction::LONG);
    strategy.during(tickAt(kWinterMidnight, minutes{600}, 110050, 110040), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("SessionRangeBreakoutStrategy rejects malformed configuration",
          "[sessionRangeBreakout]") {
    SECTION("no OHLC timeframe") {
        auto config = makeConfig();
        config.OHLC_VARIABLES.clear();
        CHECK_THROWS_AS(SessionRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("missing SESSION_RANGE_BREAKOUT_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SESSION_RANGE_BREAKOUT_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(SessionRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("ENTRY_WINDOW_MINUTES below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SESSION_RANGE_BREAKOUT_VARIABLES
            ->ENTRY_WINDOW_MINUTES = 0;
        CHECK_THROWS_AS(SessionRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("negative BUFFER_PIPS") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SESSION_RANGE_BREAKOUT_VARIABLES->BUFFER_PIPS = -1;
        CHECK_THROWS_AS(SessionRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("window one bar short of midnight -> entry cutoff coverage") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(SessionRangeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(SessionRangeBreakoutStrategy{config}, std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips SESSION_RANGE_BREAKOUT_VARIABLES through JSON",
          "[sessionRangeBreakout]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.SESSION_RANGE_BREAKOUT_VARIABLES =
            tradingDefinitions::SessionRangeBreakoutVariables{
                .BUFFER_PIPS = 7,
                .ENTRY_WINDOW_MINUTES = 90,
                .MAX_TRADE_DURATION_MINUTES = 45};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.SESSION_RANGE_BREAKOUT_VARIABLES.has_value());
        CHECK(back.SESSION_RANGE_BREAKOUT_VARIABLES->BUFFER_PIPS == 7);
        CHECK(back.SESSION_RANGE_BREAKOUT_VARIABLES->ENTRY_WINDOW_MINUTES == 90);
        CHECK(back.SESSION_RANGE_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 45);
    }

    SECTION("absent MAX_TRADE_DURATION_MINUTES parses as disabled") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0, not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"SESSION_RANGE_BREAKOUT_VARIABLES":{"BUFFER_PIPS":2,)"
                R"("ENTRY_WINDOW_MINUTES":60}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.SESSION_RANGE_BREAKOUT_VARIABLES.has_value());
        CHECK(vars.SESSION_RANGE_BREAKOUT_VARIABLES->ENTRY_WINDOW_MINUTES == 60);
        CHECK(vars.SESSION_RANGE_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("SESSION_RANGE_BREAKOUT_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .SESSION_RANGE_BREAKOUT_VARIABLES.has_value());
    }
}
