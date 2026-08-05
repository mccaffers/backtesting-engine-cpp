#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the bar store off QuestDB
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/strategyConfig.hpp"

import fvgStrategy;
import barStore;  // bars::BarStore — the strategy reads bars from it
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// FVG timeframe: 1m bars, window derived at the ctor minimum LOOKBACK_BARS+3
// (like the sweep mapper); HTF trend timeframe: HTF_SMA_PERIOD+2 bars of
// `htfMinutes`. With everything on 1 minute the store dedups both consumers
// into one shared series and the HTF trend reduces to "last closed close vs
// the one before it".
tradingDefinitions::StrategyConfig makeConfig(int minGapPips = 3,
                                              int lookbackBars = 2,
                                              int minGapAgeBars = 0,
                                              int htfSmaPeriod = 2,
                                              int htfMinutes = 1,
                                              int maxTradeDurationMinutes = 0) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-fvg";
    config.TRADING_VARIABLES.STRATEGY = "FvgStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = lookbackBars + 3,
                                          .OHLC_MINUTES = 1},  // FVG timeframe
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = htfSmaPeriod + 2,
                                          .OHLC_MINUTES = htfMinutes},  // HTF trend
    };
    config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES =
        tradingDefinitions::FVGStrategyVariables{
            .LOOKBACK_BARS = lookbackBars,
            .MIN_GAP_PIPS = minGapPips,
            .HTF_SMA_PERIOD = htfSmaPeriod,
            .MIN_GAP_AGE_BARS = minGapAgeBars,
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
std::optional<Direction> step(FvgStrategy& strategy, TradeManager& tm,
                              bars::BarStore& store, const PriceData& tick) {
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
// None of the feed ticks may signal.
void feedBar(FvgStrategy& strategy, TradeManager& tm, bars::BarStore& store,
             minutes base, int slot, const BarShape& bar,
             const std::string& symbol = "EURUSD") {
    const auto barStart = base + minutes{2 * slot};
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

void feedFixture(FvgStrategy& strategy, TradeManager& tm, bars::BarStore& store,
                 std::span<const BarShape> bars, minutes base = minutes{0},
                 const std::string& symbol = "EURUSD") {
    for (std::size_t slot = 0; slot < bars.size(); ++slot) {
        feedBar(strategy, tm, store, base, static_cast<int>(slot), bars[slot],
                symbol);
    }
}

// Canonical bullish fixture: flat warm bar, then c1/c2/c3 leaving a 40-point
// bullish gap [c1.high=110020, c3.low=110060], HTF uptrend (c3 closes above
// c2), and c3 closing above the gap. With the default config the window is
// one bar short of full while these feed, so the decision tick (slot 4,
// minutes{8}) is the first one evaluated — it rolls c3 closed and probes the
// gap with its ask.
constexpr std::array<BarShape, 4> kBullishBars{{
    {110000, 110000, 110000, 110000},
    {110000, 110020, 109990, 110010},  // c1
    {110010, 110120, 110005, 110110},  // c2 — the displacement bar
    {110110, 110150, 110060, 110140},  // c3
}};

// Bearish mirror: 40-point gap [c3.high=110140, c1.low=110180], HTF downtrend
// (c3 closes below c2), c3 closing below the gap. SHORT probes with the bid.
constexpr std::array<BarShape, 4> kBearishBars{{
    {110200, 110200, 110200, 110200},
    {110200, 110210, 110180, 110190},  // c1
    {110190, 110195, 110080, 110090},  // c2
    {110090, 110140, 110060, 110070},  // c3
}};

// The bearish shape shifted to AUDUSD's price level (gap [65140, 65180]) for
// the cross-symbol isolation test.
constexpr std::array<BarShape, 4> kAudBearishBars{{
    {65200, 65200, 65200, 65200},
    {65200, 65210, 65180, 65190},  // c1
    {65190, 65195, 65080, 65090},  // c2
    {65090, 65140, 65060, 65070},  // c3
}};

}  // namespace

TEST_CASE("FvgStrategy signals LONG when the ask retraces into a bullish gap",
          "[fvg]") {
    FvgStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedFixture(strategy, tm, store, kBullishBars);

    SECTION("mid-gap") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110040, 110030)) ==
              Direction::LONG);
    }

    SECTION("exactly on c3.low: LONG (upper bound inclusive)") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110060, 110050)) ==
              Direction::LONG);
    }

    SECTION("exactly on c1.high: LONG (lower bound inclusive)") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110020, 110010)) ==
              Direction::LONG);
    }
}

TEST_CASE("FvgStrategy ignores price outside the gap", "[fvg]") {
    FvgStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedFixture(strategy, tm, store, kBullishBars);

    SECTION("one point above c3.low: no signal") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{8}, 110061, 110051))
                        .has_value());
    }

    SECTION("one point below c1.high: no signal") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{8}, 110019, 110009))
                        .has_value());
    }
}

TEST_CASE("FvgStrategy respects the minimum gap size", "[fvg]") {
    // The fixture's gap is exactly 40 points = 4 pips on EURUSD (scale 10).
    SECTION("gap below MIN_GAP_PIPS: no signal") {
        FvgStrategy strategy{makeConfig(5)};
        TradeManager tm;
        auto store = makeStore(makeConfig(5));
        feedFixture(strategy, tm, store, kBullishBars);
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{8}, 110040, 110030))
                        .has_value());
    }

    SECTION("gap exactly MIN_GAP_PIPS: LONG (>= inclusive)") {
        FvgStrategy strategy{makeConfig(4)};
        TradeManager tm;
        auto store = makeStore(makeConfig(4));
        feedFixture(strategy, tm, store, kBullishBars);
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110040, 110030)) ==
              Direction::LONG);
    }
}

TEST_CASE("FvgStrategy converts MIN_GAP_PIPS per symbol scale", "[fvg]") {
    // The same 40-point bullish shape on two scales: on EURUSD (10 points per
    // pip) 40 points = 4 pips and meets a 4-pip floor; on DEUIDXEUR (100
    // points per pip) the identical shape is only 0.4 pips, so the gap is too
    // small. Same config, same bars — only the symbol differs.
    FvgStrategy strategy{makeConfig(4)};
    TradeManager tm;
    auto store = makeStore(makeConfig(4));

    SECTION("EURUSD: 40 points meets the 4-pip floor") {
        feedFixture(strategy, tm, store, kBullishBars);
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110040, 110030)) ==
              Direction::LONG);
    }

    SECTION("DEUIDXEUR: 40 points is under the 4-pip floor — no signal") {
        feedFixture(strategy, tm, store, kBullishBars, minutes{0}, "DEUIDXEUR");
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(minutes{8}, 110040, 110030, "DEUIDXEUR"))
                        .has_value());
    }
}

TEST_CASE("FvgStrategy signals SHORT when the bid retraces into a bearish gap",
          "[fvg]") {
    FvgStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedFixture(strategy, tm, store, kBearishBars);

    SECTION("mid-gap") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110170, 110160)) ==
              Direction::SHORT);
    }

    SECTION("exactly on c3.high: SHORT (lower bound inclusive)") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110150, 110140)) ==
              Direction::SHORT);
    }

    SECTION("exactly on c1.low: SHORT (upper bound inclusive)") {
        CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110190, 110180)) ==
              Direction::SHORT);
    }

    SECTION("one point above c1.low: no signal") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{8}, 110191, 110181))
                        .has_value());
    }
}

TEST_CASE("FvgStrategy trend filter blocks a counter-trend gap", "[fvg]") {
    FvgStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());

    // Bullish fixture except c3 closes BELOW c2's close (110070 < 110110):
    // the HTF reads downtrend while every other bullish condition still
    // holds (gap 40, c3 still closes above the gap: 110070 > 110060).
    feedFixture(strategy, tm, store, std::span(kBullishBars).first(3));
    feedBar(strategy, tm, store, minutes{0}, 3, {110110, 110150, 110060, 110070});

    CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{8}, 110040, 110030))
                    .has_value());
}

TEST_CASE("FvgStrategy skips a mitigated gap", "[fvg]") {
    // Six-bar window (LOOKBACK_BARS=3) with the gap one pattern old
    // (MIN_GAP_AGE_BARS=1): a bar after c3 decides the gap's fate before the
    // decision tick at minutes{10}.
    const auto config = makeConfig(3, 3, 1);

    SECTION("a later bar traded through the gap: no signal") {
        FvgStrategy strategy{config};
        TradeManager tm;
        auto store = makeStore(config);
        feedFixture(strategy, tm, store, kBullishBars);
        // Low 110015 <= c1.high 110020 — fills the whole gap.
        feedBar(strategy, tm, store, minutes{0}, 4, {110140, 110148, 110015, 110145});
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{10}, 110040, 110030))
                        .has_value());
    }

    SECTION("a later bar dipped into but not through the gap: still LONG") {
        FvgStrategy strategy{config};
        TradeManager tm;
        auto store = makeStore(config);
        feedFixture(strategy, tm, store, kBullishBars);
        // Low 110030 stays above c1.high 110020 — a partial fill leaves the
        // gap live.
        feedBar(strategy, tm, store, minutes{0}, 4, {110140, 110148, 110030, 110145});
        CHECK(step(strategy, tm, store, tickAt(minutes{10}, 110040, 110030)) ==
              Direction::LONG);
    }
}

TEST_CASE("FvgStrategy MIN_GAP_AGE_BARS widens the scan backward", "[fvg]") {
    // Same six-bar layout with a clean bar after c3 (never touches the gap):
    // the gap is one pattern old at the decision tick, so it is only
    // reachable when the age allows scanning past the newest pattern.
    SECTION("age 0: only the newest pattern is examined — no signal") {
        const auto config = makeConfig(3, 3, 0);
        FvgStrategy strategy{config};
        TradeManager tm;
        auto store = makeStore(config);
        feedFixture(strategy, tm, store, kBullishBars);
        feedBar(strategy, tm, store, minutes{0}, 4, {110140, 110148, 110100, 110145});
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{10}, 110040, 110030))
                        .has_value());
    }

    SECTION("age 1: the previous pattern is reached — LONG") {
        const auto config = makeConfig(3, 3, 1);
        FvgStrategy strategy{config};
        TradeManager tm;
        auto store = makeStore(config);
        feedFixture(strategy, tm, store, kBullishBars);
        feedBar(strategy, tm, store, minutes{0}, 4, {110140, 110148, 110100, 110145});
        CHECK(step(strategy, tm, store, tickAt(minutes{10}, 110040, 110030)) ==
              Direction::LONG);
    }
}

// The last-closed-bar gate ("close still above the gap") can only fail on the
// newest pattern when c3 closes exactly on the gap edge — pin that boundary.
// The HTF must stay in an uptrend while c3 closes low, which needs genuinely
// separated timeframes: four 60m warm bars stepping up 10 points (strictly
// below the 3-pip = 30-point gap floor, so the single-tick 1m warm bars can
// never form an accidental gap) hold the trend up, and the crafted 1m cluster
// then lives inside the in-progress 4th 60m bar, which the closed-bar SMA
// never reads.
TEST_CASE("FvgStrategy requires the last close outside the gap", "[fvg]") {
    const auto config = makeConfig(3, 2, 0, 2, 60);

    auto warmUp = [](FvgStrategy& strategy, TradeManager& tm,
                     bars::BarStore& store) {
        const std::array<std::pair<minutes, std::int32_t>, 4> warmTicks{{
            {minutes{0}, 110000},
            {minutes{61}, 110010},
            {minutes{122}, 110020},
            {minutes{183}, 110030},
        }};
        for (const auto& [offset, ask] : warmTicks) {
            CHECK_FALSE(step(strategy, tm, store,
                             tickAt(offset, ask, ask - 10))
                            .has_value());
        }
    };

    SECTION("c3 closes exactly on c3.low: no signal (strictly greater)") {
        FvgStrategy strategy{config};
        TradeManager tm;
        auto store = makeStore(config);
        warmUp(strategy, tm, store);
        feedFixture(strategy, tm, store, std::span(kBullishBars).first(3),
                    minutes{185});
        feedBar(strategy, tm, store, minutes{185}, 3, {110110, 110150, 110060, 110060});
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(minutes{185} + minutes{8}, 110040, 110030))
                        .has_value());
    }

    SECTION("c3 closes one point above the gap: LONG") {
        FvgStrategy strategy{config};
        TradeManager tm;
        auto store = makeStore(config);
        warmUp(strategy, tm, store);
        feedFixture(strategy, tm, store, std::span(kBullishBars).first(3),
                    minutes{185});
        feedBar(strategy, tm, store, minutes{185}, 3, {110110, 110150, 110060, 110070});
        CHECK(step(strategy, tm, store,
                   tickAt(minutes{185} + minutes{8}, 110040, 110030)) ==
              Direction::LONG);
    }
}

TEST_CASE("FvgStrategy keeps per-symbol state isolated", "[fvg]") {
    FvgStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());

    // Interleave a bullish EURUSD with a bearish AUDUSD at a very different
    // price level. If either symbol's ticks leaked into the other's bars, the
    // gaps and trend filters below would be wildly wrong.
    for (int slot = 0; slot < 4; ++slot) {
        feedBar(strategy, tm, store, minutes{0}, slot,
                kBullishBars[static_cast<std::size_t>(slot)]);
        feedBar(strategy, tm, store, minutes{0}, slot,
                kAudBearishBars[static_cast<std::size_t>(slot)], "AUDUSD");
    }

    CHECK(step(strategy, tm, store, tickAt(minutes{8}, 110040, 110030)) ==
          Direction::LONG);
    // AUDUSD's bearish gap is [65140, 65180]; the bid probes it.
    CHECK(step(strategy, tm, store,
               tickAt(minutes{8} + seconds{30}, 65170, 65160, "AUDUSD")) ==
          Direction::SHORT);
}

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — same harness as the NyOpenRangeBreakoutStrategy cap tests.
TEST_CASE("FvgStrategy closes trades past the max duration via during()",
          "[fvg]") {
    FvgStrategy strategy{makeConfig(3, 2, 0, 2, 1, 60)};
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
TEST_CASE("FvgStrategy max duration of zero disables the exit", "[fvg]") {
    FvgStrategy strategy{makeConfig()};
    TradeManager tm;

    tm.openTrade(tickAt(seconds{0}, 110000, 109990), 1, Direction::LONG);
    strategy.during(tickAt(minutes{600}, 110050, 110040), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("FvgStrategy rejects malformed configuration", "[fvg]") {
    SECTION("fewer than two OHLC timeframes") {
        auto config = makeConfig();
        config.OHLC_VARIABLES.resize(1);
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("missing FVG_STRATEGY_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("LOOKBACK_BARS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES->LOOKBACK_BARS = 0;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("MIN_GAP_PIPS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES->MIN_GAP_PIPS = 0;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("HTF_SMA_PERIOD below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES->HTF_SMA_PERIOD = 0;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("negative MIN_GAP_AGE_BARS") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES->MIN_GAP_AGE_BARS = -1;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("negative MAX_TRADE_DURATION_MINUTES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.FVG_STRATEGY_VARIABLES
            ->MAX_TRADE_DURATION_MINUTES = -1;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("FVG window one bar short of LOOKBACK_BARS + 3") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("HTF window one bar short of HTF_SMA_PERIOD + 2") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[1].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[1].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(FvgStrategy{config}, std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips FVG_STRATEGY_VARIABLES through JSON",
          "[fvg]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.FVG_STRATEGY_VARIABLES = tradingDefinitions::FVGStrategyVariables{
            .LOOKBACK_BARS = 12,
            .MIN_GAP_PIPS = 25,
            .HTF_SMA_PERIOD = 30,
            .MIN_GAP_AGE_BARS = 4,
            .MAX_TRADE_DURATION_MINUTES = 45};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.FVG_STRATEGY_VARIABLES.has_value());
        CHECK(back.FVG_STRATEGY_VARIABLES->LOOKBACK_BARS == 12);
        CHECK(back.FVG_STRATEGY_VARIABLES->MIN_GAP_PIPS == 25);
        CHECK(back.FVG_STRATEGY_VARIABLES->HTF_SMA_PERIOD == 30);
        CHECK(back.FVG_STRATEGY_VARIABLES->MIN_GAP_AGE_BARS == 4);
        CHECK(back.FVG_STRATEGY_VARIABLES->MAX_TRADE_DURATION_MINUTES == 45);
    }

    SECTION("absent MIN_GAP_AGE_BARS parses as newest-pattern-only") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0, not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"FVG_STRATEGY_VARIABLES":{"LOOKBACK_BARS":12,)"
                R"("MIN_GAP_PIPS":25,"HTF_SMA_PERIOD":30}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.FVG_STRATEGY_VARIABLES.has_value());
        CHECK(vars.FVG_STRATEGY_VARIABLES->LOOKBACK_BARS == 12);
        CHECK(vars.FVG_STRATEGY_VARIABLES->MIN_GAP_AGE_BARS == 0);
        // Same doctrine for the cap: absent = 0 = disabled, the pre-cap
        // behaviour those persisted winners were scored with.
        CHECK(vars.FVG_STRATEGY_VARIABLES->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("FVG_STRATEGY_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .FVG_STRATEGY_VARIABLES.has_value());
    }
}
