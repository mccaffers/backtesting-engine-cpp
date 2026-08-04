#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

import squeezeBreakoutStrategy;
import barStore;  // bars::BarStore — the strategy reads bars from it
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// Signal timeframe: 1m bars, window derived at the ctor minimum
// VALID_BARS + max(1, NR_LOOKBACK - 1) + 1 (like the sweep mapper). Trend
// timeframe: 4 bars of 1m (EMA period 2) — the store dedups both consumers
// into one shared series and each reads its own tail, like production.
tradingDefinitions::StrategyConfig makeConfig(int nrLookback = 0,
                                              int validBars = 1,
                                              int bufferPips = 0,
                                              int maxTradeDurationMinutes = 0) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-squeeze";
    config.TRADING_VARIABLES.STRATEGY = "SqueezeBreakoutStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{
            .OHLC_COUNT = validBars + std::max(1, nrLookback - 1) + 1,
            .OHLC_MINUTES = 1},  // signal timeframe
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = 4,
                                          .OHLC_MINUTES = 1},  // trend
    };
    config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES =
        tradingDefinitions::SqueezeBreakoutVariables{
            .NR_LOOKBACK = nrLookback,
            .VALID_BARS = validBars,
            .BUFFER_PIPS = bufferPips,
            .MAX_TRADE_DURATION_MINUTES = maxTradeDurationMinutes};
    return config;
}

PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 std::int32_t bid, const std::string& symbol = "EURUSD") {
    return PriceData(ask, bid, t0 + offset, symbol);
}

// The loop owner's role in miniature: register every configured timeframe
// (same-duration entries dedup into one shared series with the larger window;
// each consumer reads its own tail, like production).
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
std::optional<Direction> step(SqueezeBreakoutStrategy& strategy,
                              TradeManager& tm, bars::BarStore& store,
                              const PriceData& tick) {
    store.update(tick);
    const auto signal = strategy.decide(tick, store);
    strategy.during(tick, store, tm);
    return signal;
}

// The exact OHLC a crafted 1m bar should end up with.
struct BarShape {
    std::int32_t o, h, l, c;
};

// Four asks inside one 1m bar, fed open/high/low/close: the first tick sets
// the open, later ones only extend the extremes and overwrite the close, so
// the bar lands on exactly this shape. Bars sit 2 minutes apart (slot = bar
// index) so the NEXT bar's first tick rolls this one. Bars are built from the
// ask; the bid trails 10 points and is irrelevant until a SHORT signal tick.
// None of the feed ticks may signal (the trend window is one bar short of
// full until the decision tick arrives).
void feedBar(SqueezeBreakoutStrategy& strategy, TradeManager& tm,
             bars::BarStore& store, int slot, const BarShape& bar,
             const std::string& symbol = "EURUSD") {
    const auto barStart = minutes{2 * slot};
    const std::array<std::pair<seconds, std::int32_t>, 4> ticks{{
        {seconds{0}, bar.o},
        {seconds{15}, bar.h},
        {seconds{30}, bar.l},
        {seconds{45}, bar.c},
    }};
    for (const auto& [offset, ask] : ticks) {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(barStart + offset, ask, ask - 10, symbol))
                        .has_value());
    }
}

// Canonical inside-bar fixture: a flat warm bar (its level picks the trend
// regime), a wide mother bar, then an inside bar whose range [109945, 109955]
// sits inside the mother's [109900, 109960]. The decision tick at minutes{6}
// rolls the inside bar closed and probes its levels.
constexpr BarShape kWarmLow{109940, 109940, 109940, 109940};    // uptrend
constexpr BarShape kWarmHigh{110100, 110100, 110100, 110100};   // downtrend
constexpr BarShape kMotherBar{109940, 109960, 109900, 109950};
constexpr BarShape kInsideBar{109950, 109955, 109945, 109950};

}  // namespace

TEST_CASE("SqueezeBreakoutStrategy trades the break of an inside bar",
          "[squeezeBreakout]") {
    SqueezeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedBar(strategy, tm, store, 0, kWarmLow);
    feedBar(strategy, tm, store, 1, kMotherBar);
    feedBar(strategy, tm, store, 2, kInsideBar);

    SECTION("bid above the inside bar's high in an uptrend: LONG") {
        CHECK(step(strategy, tm, store, tickAt(minutes{6}, 109975, 109965)) ==
              Direction::LONG);
    }

    SECTION("bid exactly on the inside bar's high: no signal (strictly above)") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{6}, 109965, 109955))
                        .has_value());
    }
}

TEST_CASE("SqueezeBreakoutStrategy shorts the downside break in a downtrend",
          "[squeezeBreakout]") {
    SqueezeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    // The high warm bar keeps the trend EMA overhead, so the downside break
    // reads as trend-aligned.
    feedBar(strategy, tm, store, 0, kWarmHigh);
    feedBar(strategy, tm, store, 1, {109960, 110000, 109940, 109950});
    feedBar(strategy, tm, store, 2, kInsideBar);

    CHECK(step(strategy, tm, store, tickAt(minutes{6}, 109935, 109925)) ==
          Direction::SHORT);
}

TEST_CASE("SqueezeBreakoutStrategy trend filter blocks a counter-trend break",
          "[squeezeBreakout]") {
    SqueezeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    // Same inside-bar setup as the LONG case, but the warm bar sits far
    // OVERHEAD: the upside break clears the pattern's high yet stays below
    // the trailing EMA, so the macro filter reads downtrend and refuses it.
    feedBar(strategy, tm, store, 0, kWarmHigh);
    feedBar(strategy, tm, store, 1, kMotherBar);
    feedBar(strategy, tm, store, 2, kInsideBar);

    CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{6}, 109970, 109960))
                    .has_value());
}

TEST_CASE("SqueezeBreakoutStrategy ignores a bar that is not inside its mother",
          "[squeezeBreakout]") {
    SqueezeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedBar(strategy, tm, store, 0, kWarmLow);
    feedBar(strategy, tm, store, 1, kMotherBar);
    // High 109965 pokes above the mother's 109960 — no contraction.
    feedBar(strategy, tm, store, 2, {109950, 109965, 109945, 109950});

    CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{6}, 109980, 109970))
                    .has_value());
}

TEST_CASE("SqueezeBreakoutStrategy honours the pattern validity window",
          "[squeezeBreakout]") {
    // A fourth bar follows the inside bar without being a pattern itself: its
    // high pokes ONE point above the inside bar's, which breaks the
    // contraction without breaching the pattern's levels mid-feed (while this
    // bar is in progress the inside bar is still the newest closed pattern,
    // so any feed tick beyond its high would legitimately signal). Whether
    // the aged inside bar still supplies levels afterwards is exactly
    // VALID_BARS.
    const BarShape pokeAfter{109950, 109956, 109946, 109950};

    SECTION("VALID_BARS = 1: the aged pattern no longer trades") {
        SqueezeBreakoutStrategy strategy{makeConfig(0, 1)};
        TradeManager tm;
        auto store = makeStore(makeConfig(0, 1));
        feedBar(strategy, tm, store, 0, kWarmLow);
        feedBar(strategy, tm, store, 1, kMotherBar);
        feedBar(strategy, tm, store, 2, kInsideBar);
        feedBar(strategy, tm, store, 3, pokeAfter);

        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{8}, 109975, 109965))
                        .has_value());
    }

    SECTION("VALID_BARS = 3: the pattern one bar back still trades") {
        SqueezeBreakoutStrategy strategy{makeConfig(0, 3)};
        TradeManager tm;
        auto store = makeStore(makeConfig(0, 3));
        feedBar(strategy, tm, store, 0, kWarmLow);
        feedBar(strategy, tm, store, 1, kMotherBar);
        feedBar(strategy, tm, store, 2, kInsideBar);
        feedBar(strategy, tm, store, 3, pokeAfter);

        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 109975, 109965)) ==
              Direction::LONG);
    }
}

TEST_CASE("SqueezeBreakoutStrategy NR mode requires the strictly narrowest range",
          "[squeezeBreakout]") {
    SECTION("narrowest of the last 3: LONG on the break") {
        SqueezeBreakoutStrategy strategy{makeConfig(3)};
        TradeManager tm;
        auto store = makeStore(makeConfig(3));
        feedBar(strategy, tm, store, 0, {109950, 110010, 109950, 109980});  // range 60
        feedBar(strategy, tm, store, 1, {109980, 110000, 109960, 109980});  // range 40
        feedBar(strategy, tm, store, 2, {109980, 109990, 109970, 109980});  // range 20

        CHECK(step(strategy, tm, store, tickAt(minutes{6}, 110011, 110001)) ==
              Direction::LONG);
    }

    SECTION("wider than an earlier bar: no pattern, no signal") {
        SqueezeBreakoutStrategy strategy{makeConfig(3)};
        TradeManager tm;
        auto store = makeStore(makeConfig(3));
        feedBar(strategy, tm, store, 0, {109950, 110010, 109950, 109980});  // range 60
        feedBar(strategy, tm, store, 1, {109980, 110000, 109960, 109980});  // range 40
        feedBar(strategy, tm, store, 2, {109980, 110003, 109958, 109980});  // range 45

        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{6}, 110014, 110004))
                        .has_value());
    }
}

TEST_CASE("SqueezeBreakoutStrategy pads the levels with BUFFER_PIPS",
          "[squeezeBreakout]") {
    // 2 pips = 20 points on EURUSD: the padded inside-bar high sits at 109975.
    SqueezeBreakoutStrategy strategy{makeConfig(0, 1, 2)};
    TradeManager tm;
    auto store = makeStore(makeConfig(0, 1, 2));
    feedBar(strategy, tm, store, 0, kWarmLow);
    feedBar(strategy, tm, store, 1, kMotherBar);
    feedBar(strategy, tm, store, 2, kInsideBar);

    SECTION("a poke through the raw high but not the padding: no signal") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{6}, 109985, 109975))
                        .has_value());
    }

    SECTION("clearing the padded high: LONG") {
        CHECK(step(strategy, tm, store, tickAt(minutes{6}, 109986, 109976)) ==
              Direction::LONG);
    }
}

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — same harness as the NyOpenRangeBreakoutStrategy cap tests.
TEST_CASE("SqueezeBreakoutStrategy closes trades past the max duration via during()",
          "[squeezeBreakout]") {
    SqueezeBreakoutStrategy strategy{makeConfig(0, 1, 0, 60)};
    TradeManager tm;

    SECTION("LONG past the cap closes at the bid") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050, 110040),
                        bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110040);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("SHORT past the cap closes at the ask") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::SHORT);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050, 110040),
                        bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110050);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("exactly at the cap stays open (strictly greater)") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109998), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60}, 110050, 110040), bars::BarStore{},
                        tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
}

// With the cap at 0 (the pre-cap winner-config default) exits stay owned by
// Operations via SL/TP — during() must not touch open positions.
TEST_CASE("SqueezeBreakoutStrategy max duration of zero disables the exit",
          "[squeezeBreakout]") {
    SqueezeBreakoutStrategy strategy{makeConfig()};
    TradeManager tm;

    tm.openTrade(tickAt(seconds{0}, 110000, 109990), 1, Direction::LONG);
    strategy.during(tickAt(minutes{600}, 110050, 110040), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("SqueezeBreakoutStrategy rejects malformed configuration",
          "[squeezeBreakout]") {
    SECTION("fewer than two OHLC timeframes") {
        auto config = makeConfig();
        config.OHLC_VARIABLES.resize(1);
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("missing SQUEEZE_BREAKOUT_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("NR_LOOKBACK of 1 (degenerate — every bar matches)") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES->NR_LOOKBACK = 1;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("negative NR_LOOKBACK") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES->NR_LOOKBACK = -1;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("VALID_BARS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES->VALID_BARS = 0;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("negative BUFFER_PIPS") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES->BUFFER_PIPS = -1;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("negative MAX_TRADE_DURATION_MINUTES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.SQUEEZE_BREAKOUT_VARIABLES
            ->MAX_TRADE_DURATION_MINUTES = -1;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("signal window one bar short of the scan") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("trend window below 2") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[1].OHLC_COUNT = 1;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[1].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(SqueezeBreakoutStrategy{config}, std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips SQUEEZE_BREAKOUT_VARIABLES through JSON",
          "[squeezeBreakout]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.SQUEEZE_BREAKOUT_VARIABLES =
            tradingDefinitions::SqueezeBreakoutVariables{
                .NR_LOOKBACK = 7,
                .VALID_BARS = 3,
                .BUFFER_PIPS = 2,
                .MAX_TRADE_DURATION_MINUTES = 45};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.SQUEEZE_BREAKOUT_VARIABLES.has_value());
        CHECK(back.SQUEEZE_BREAKOUT_VARIABLES->NR_LOOKBACK == 7);
        CHECK(back.SQUEEZE_BREAKOUT_VARIABLES->VALID_BARS == 3);
        CHECK(back.SQUEEZE_BREAKOUT_VARIABLES->BUFFER_PIPS == 2);
        CHECK(back.SQUEEZE_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 45);
    }

    SECTION("absent NR_LOOKBACK parses as inside-bar mode") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0 (a VALID mode), not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"SQUEEZE_BREAKOUT_VARIABLES":{"VALID_BARS":2,)"
                R"("BUFFER_PIPS":1}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.SQUEEZE_BREAKOUT_VARIABLES.has_value());
        CHECK(vars.SQUEEZE_BREAKOUT_VARIABLES->NR_LOOKBACK == 0);
        CHECK(vars.SQUEEZE_BREAKOUT_VARIABLES->VALID_BARS == 2);
        // Same doctrine for the cap: absent = 0 = disabled, the pre-cap
        // behaviour those persisted winners were scored with.
        CHECK(vars.SQUEEZE_BREAKOUT_VARIABLES->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("SQUEEZE_BREAKOUT_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .SQUEEZE_BREAKOUT_VARIABLES.has_value());
    }
}
