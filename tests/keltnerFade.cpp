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

import keltnerFadeStrategy;
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
// BAND_SMA_PERIOD + 2 (like the sweep mapper): period closed bars for the
// SMA, period + 1 for the ATR, plus the in-progress last bar.
tradingDefinitions::StrategyConfig makeConfig(int bandSmaPeriod = 4,
                                              int bandAtrMultTenths = 15,
                                              int ohlcMinutes = 1,
                                              int maxTradeDurationMinutes = 0) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-keltner";
    config.TRADING_VARIABLES.STRATEGY = "KeltnerFadeStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{.OHLC_COUNT = bandSmaPeriod + 2,
                                          .OHLC_MINUTES = ohlcMinutes},
    };
    config.STRATEGY_VARIABLES.KELTNER_FADE_VARIABLES =
        tradingDefinitions::KeltnerFadeVariables{
            .BAND_SMA_PERIOD = bandSmaPeriod,
            .BAND_ATR_MULT_TENTHS = bandAtrMultTenths,
            .MAX_TRADE_DURATION_MINUTES = maxTradeDurationMinutes};
    return config;
}

PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 std::int32_t bid, const std::string& symbol = "EURUSD") {
    return PriceData(ask, bid, t0 + offset, symbol);
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
std::optional<Direction> step(KeltnerFadeStrategy& strategy, TradeManager& tm,
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
void feedBar(KeltnerFadeStrategy& strategy, TradeManager& tm,
             bars::BarStore& store, minutes base, int slot, const BarShape& bar,
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

// Six identical bars around a 110000 centre with a 20-point true range:
// SMA(4) of the closed closes = 110000, ATR(4) = 20, so with
// BAND_ATR_MULT_TENTHS = 15 the band half-width is 1.5 x 20 = 30 points and
// the band is [109970, 110030]. Every feed tick sits inside it. The decision
// ticks land at minutes{12} (slot 6), whose first tick rolls the last warm
// bar closed — the band above is exactly what they are judged against.
void feedWarmBand(KeltnerFadeStrategy& strategy, TradeManager& tm,
                  bars::BarStore& store) {
    for (int slot = 0; slot < 6; ++slot) {
        feedBar(strategy, tm, store, minutes{0}, slot,
                {110000, 110010, 109990, 110000});
    }
}

}  // namespace

TEST_CASE("KeltnerFadeStrategy fades a stretch below the lower band", "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedWarmBand(strategy, tm, store);

    SECTION("ask one point below the band: LONG") {
        CHECK(step(strategy, tm, store, tickAt(minutes{12}, 109969, 109959)) ==
              Direction::LONG);
    }

    SECTION("ask exactly on the band: no signal (strictly outside)") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{12}, 109970, 109960))
                        .has_value());
    }
}

TEST_CASE("KeltnerFadeStrategy fades a stretch above the upper band", "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedWarmBand(strategy, tm, store);

    SECTION("bid one point above the band: SHORT") {
        CHECK(step(strategy, tm, store, tickAt(minutes{12}, 110041, 110031)) ==
              Direction::SHORT);
    }

    SECTION("bid exactly on the band: no signal (strictly outside)") {
        CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{12}, 110040, 110030))
                        .has_value());
    }
}

TEST_CASE("KeltnerFadeStrategy judges the trade's own fill side", "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedWarmBand(strategy, tm, store);

    // The bid sits below the lower band but the ask (what a LONG would pay)
    // does not — the spread must not flatter the stretch.
    CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{12}, 109975, 109965))
                    .has_value());
}

TEST_CASE("KeltnerFadeStrategy builds the band from closed bars only", "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedWarmBand(strategy, tm, store);

    // First decision tick sets the in-progress bar's close to 109975 (inside
    // the band — no signal). Had that close leaked into the SMA, the centre
    // would sag to 109993.75 and the lower band to ~109964, hiding the LONG
    // below. The closed-bar band keeps its edge at 109970, so the second tick
    // still signals.
    CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{12}, 109975, 109965))
                    .has_value());
    CHECK(step(strategy, tm, store,
               tickAt(minutes{12} + seconds{30}, 109969, 109959)) ==
          Direction::LONG);
}

TEST_CASE("KeltnerFadeStrategy treats a dead-flat market as untradeable", "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());

    // Every bar is a single price: all true ranges are 0, ATR = 0, and a
    // zero-width band would fade every tick of noise — decide() must refuse.
    for (int slot = 0; slot < 6; ++slot) {
        feedBar(strategy, tm, store, minutes{0}, slot,
                {110000, 110000, 110000, 110000});
    }
    CHECK_FALSE(step(strategy, tm, store, tickAt(minutes{12}, 109900, 109890))
                    .has_value());
}

TEST_CASE("KeltnerFadeStrategy keeps per-symbol state isolated", "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedWarmBand(strategy, tm, store);

    // AUDUSD's history is only the single bar this tick opens — nowhere near
    // a full window, so no signal at any price: the EURUSD band must not
    // leak across symbols.
    CHECK_FALSE(step(strategy, tm, store,
                     tickAt(minutes{12}, 109969, 109959, "AUDUSD"))
                    .has_value());
    // EURUSD itself still signals (interleaving did not disturb its series).
    CHECK(step(strategy, tm, store,
               tickAt(minutes{12} + seconds{30}, 109969, 109959)) ==
          Direction::LONG);
}

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — same harness as the SessionRangeBreakoutStrategy cap tests.
TEST_CASE("KeltnerFadeStrategy closes trades past the max duration via during()",
          "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig(4, 15, 1, 60)};
    TradeManager tm;

    SECTION("LONG past the cap closes at the bid") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109990), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050, 110040), bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110040);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("SHORT past the cap closes at the ask") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109990), 1, Direction::SHORT);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050, 110040), bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110050);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("exactly at the cap stays open (strictly greater)") {
        tm.openTrade(tickAt(seconds{0}, 110000, 109990), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60}, 110050, 110040), bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
}

// With the cap disabled (0, the default) exits stay owned by Operations via
// SL/TP — during() must not touch open positions, otherwise the sweep's
// stop/limit parameters stop being the only exit mechanism under test.
TEST_CASE("KeltnerFadeStrategy max duration of zero disables the exit",
          "[keltnerFade]") {
    KeltnerFadeStrategy strategy{makeConfig()};
    TradeManager tm;

    tm.openTrade(tickAt(seconds{0}, 110000, 109990), 1, Direction::LONG);
    strategy.during(tickAt(minutes{600}, 110050, 110040), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("KeltnerFadeStrategy rejects malformed configuration", "[keltnerFade]") {
    SECTION("no OHLC timeframe") {
        auto config = makeConfig();
        config.OHLC_VARIABLES.clear();
        CHECK_THROWS_AS(KeltnerFadeStrategy{config}, std::invalid_argument);
    }

    SECTION("missing KELTNER_FADE_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.KELTNER_FADE_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(KeltnerFadeStrategy{config}, std::invalid_argument);
    }

    SECTION("BAND_SMA_PERIOD below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.KELTNER_FADE_VARIABLES->BAND_SMA_PERIOD = 0;
        CHECK_THROWS_AS(KeltnerFadeStrategy{config}, std::invalid_argument);
    }

    SECTION("BAND_ATR_MULT_TENTHS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.KELTNER_FADE_VARIABLES->BAND_ATR_MULT_TENTHS = 0;
        CHECK_THROWS_AS(KeltnerFadeStrategy{config}, std::invalid_argument);
    }

    SECTION("window one bar short of BAND_SMA_PERIOD + 2") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(KeltnerFadeStrategy{config}, std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(KeltnerFadeStrategy{config}, std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips KELTNER_FADE_VARIABLES through JSON",
          "[keltnerFade]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.KELTNER_FADE_VARIABLES = tradingDefinitions::KeltnerFadeVariables{
            .BAND_SMA_PERIOD = 20,
            .BAND_ATR_MULT_TENTHS = 25,
            .MAX_TRADE_DURATION_MINUTES = 90};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.KELTNER_FADE_VARIABLES.has_value());
        CHECK(back.KELTNER_FADE_VARIABLES->BAND_SMA_PERIOD == 20);
        CHECK(back.KELTNER_FADE_VARIABLES->BAND_ATR_MULT_TENTHS == 25);
        CHECK(back.KELTNER_FADE_VARIABLES->MAX_TRADE_DURATION_MINUTES == 90);
    }

    SECTION("absent MAX_TRADE_DURATION_MINUTES parses as disabled") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0, not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"KELTNER_FADE_VARIABLES":{"BAND_SMA_PERIOD":20,)"
                R"("BAND_ATR_MULT_TENTHS":25}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.KELTNER_FADE_VARIABLES.has_value());
        CHECK(vars.KELTNER_FADE_VARIABLES->BAND_SMA_PERIOD == 20);
        CHECK(vars.KELTNER_FADE_VARIABLES->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("KELTNER_FADE_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .KELTNER_FADE_VARIABLES.has_value());
    }
}
