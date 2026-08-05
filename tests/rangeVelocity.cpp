// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// RangeVelocityStrategy: velocity momentum on range bars. The fixture makes
// every timing semantic hand-computable: a 2-tick rolling window at 100% with
// every tick stepping exactly +/-10 points locks every bar's threshold at 10,
// so EVERY bar is exactly two ticks — an opening tick and a breach tick whose
// step sign is the bar's direction. The successor's opening tick reuses the
// breach tick's timestamp (the bar construct is clock-free; only order
// matters), so a bar's open-to-open duration equals its scripted formation
// gap, which also equals the newest bar's tick-measured duration.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>  // setenv — keep the store off QuestDB
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "shared/tradingDefinitions/strategyConfig.hpp"

import rangeVelocityStrategy;
import barStore;         // bars::BarStore — the strategy reads range bars from it
import priceData;
import rangeBarBuilder;  // rangebar::RangeBarSpec — series registration
import trade;
import tradeManager;

namespace {

using std::chrono::hours;
using std::chrono::microseconds;
using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point t0 =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5} + std::chrono::hours{9};

// Plain tick for the direct during() calls (time-cap tests). Bid rides 2
// points under the ask, so a LONG's exit-side close price is ask - 2.
PriceData tickAt(std::chrono::system_clock::duration offset, std::int32_t ask,
                 const std::string& symbol = "EURUSD") {
    return PriceData(ask, ask - 2, t0 + offset, symbol);
}

// Defaults match the hand-computation recipe: K=2 run bars, M=3 baseline
// bars, ratio 100 ("at the norm" passes), E=2 exit bars, time cap off,
// RANGE_COUNT 7 >= max(K+M, E).
tradingDefinitions::StrategyConfig makeConfig(const int runBars = 2,
                                              const int speedLookback = 3,
                                              const int ratio = 100,
                                              const int exitRun = 2,
                                              const int maxDurationMinutes = 0,
                                              const int rangeCount = 7,
                                              const int tickWindow = 2,
                                              const int percent = 100) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "range-velocity-test";
    config.TRADING_VARIABLES.STRATEGY = "RangeVelocityStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 25;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 50;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.RANGE_VARIABLES = {{.RANGE_ATR_TICK_WINDOW = tickWindow,
                               .RANGE_ATR_PERCENT = percent,
                               .RANGE_COUNT = rangeCount}};
    config.STRATEGY_VARIABLES.RANGE_VELOCITY_VARIABLES =
        tradingDefinitions::RangeVelocityVariables{
            .RUN_BARS = runBars,
            .SPEED_LOOKBACK_BARS = speedLookback,
            .SPEED_RATIO_PERCENT = ratio,
            .EXIT_RUN_BARS = exitRun,
            .MAX_TRADE_DURATION_MINUTES = maxDurationMinutes};
    return config;
}

bars::BarStore makeStore(const int rangeCount = 7) {
    setenv("OHLC_PREPOPULATE", "0", 1);  // hermetic: no QuestDB warm-up query
    bars::BarStore store;
    store.registerRangeSeries(rangebar::RangeBarSpec{
        .atrTickWindow = 2, .atrPercent = 100, .count = rangeCount});
    return store;
}

// One tick in run-loop order: shared bars first, then decide, then the
// management hook — mirroring runTicks exactly.
std::optional<Direction> step(RangeVelocityStrategy& strategy, TradeManager& tm,
                              bars::BarStore& store, const PriceData& tick) {
    store.update(tick);
    const auto signal = strategy.decide(tick, store);
    strategy.during(tick, store, tm);
    return signal;
}

// Per-symbol scripted price stream. `lastBreach` is kept so tests can open
// trades at exactly the tick a signal fired on, the way runTicks would.
struct SymbolStream {
    std::string symbol = "EURUSD";
    std::int32_t price = 100000;
    std::chrono::system_clock::time_point clock = t0;
    bool warmed = false;
    PriceData lastBreach{};
};

// Emits one complete range bar of direction `dir` (+1 up, -1 down) taking
// `formation` of tick time, and returns the BREACH tick's decide() result.
// The opening tick can never signal (the just-closed gate is down), which is
// asserted inline. The very first call emits one extra warming tick — a
// 2-tick window needs one predecessor before bar #1 can open.
std::optional<Direction> emitBar(RangeVelocityStrategy& strategy,
                                 TradeManager& tm, bars::BarStore& store,
                                 SymbolStream& s, const int dir,
                                 const std::chrono::microseconds formation) {
    if (!s.warmed) {
        CHECK_FALSE(step(strategy, tm, store,
                         PriceData(s.price, s.price - 2, s.clock, s.symbol))
                        .has_value());
        s.warmed = true;
    }
    s.price += 10 * dir;  // opening tick: timestamp of the previous breach
    CHECK_FALSE(step(strategy, tm, store,
                     PriceData(s.price, s.price - 2, s.clock, s.symbol))
                    .has_value());
    s.price += 10 * dir;  // breach tick: the bar's direction and duration
    s.clock += formation;
    s.lastBreach = PriceData(s.price, s.price - 2, s.clock, s.symbol);
    return step(strategy, tm, store, s.lastBreach);
}

}  // namespace

TEST_CASE("RangeVelocityStrategy rejects malformed configuration",
          "[rangeVelocity]") {
    SECTION("empty RANGE_VARIABLES") {
        auto config = makeConfig();
        config.RANGE_VARIABLES.clear();
        CHECK_THROWS_AS(RangeVelocityStrategy{config}, std::invalid_argument);
    }
    SECTION("zero-sentinel fields in RANGE_VARIABLES[0]") {
        CHECK_THROWS_AS(
            RangeVelocityStrategy{makeConfig(2, 3, 100, 2, 0, 7, 0, 100)},
            std::invalid_argument);
        CHECK_THROWS_AS(
            RangeVelocityStrategy{makeConfig(2, 3, 100, 2, 0, 7, 2, 0)},
            std::invalid_argument);
        CHECK_THROWS_AS(
            RangeVelocityStrategy{makeConfig(2, 3, 100, 2, 0, 0)},
            std::invalid_argument);
    }
    SECTION("missing RANGE_VELOCITY_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.RANGE_VELOCITY_VARIABLES = std::nullopt;
        CHECK_THROWS_AS(RangeVelocityStrategy{config}, std::invalid_argument);
    }
    SECTION("non-positive strategy knobs") {
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(0)},
                        std::invalid_argument);
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(2, 0)},
                        std::invalid_argument);
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(2, 3, 0)},
                        std::invalid_argument);
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(2, 3, 100, 0)},
                        std::invalid_argument);
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(2, 3, 100, 2, -1)},
                        std::invalid_argument);
    }
    SECTION("RANGE_COUNT below the provable window bound") {
        // K + M dominates: 2 + 3 = 5, count 4 is one short.
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(2, 3, 100, 2, 0, 4)},
                        std::invalid_argument);
        // EXIT_RUN_BARS dominates: max(1 + 1, 7) = 7, count 6 is one short.
        CHECK_THROWS_AS(RangeVelocityStrategy{makeConfig(1, 1, 100, 7, 0, 6)},
                        std::invalid_argument);
        // Exactly at the bound constructs.
        CHECK_NOTHROW(RangeVelocityStrategy{makeConfig(2, 3, 100, 2, 0, 5)});
    }
}

TEST_CASE("silent until RUN_BARS + SPEED_LOOKBACK_BARS bars have closed",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    // Four fast up bars: run and speed would qualify, but the baseline
    // cannot exist yet — every breach stays silent while n < K + M = 5.
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(emitBar(strategy, tm, store, s, +1, seconds{5}).has_value());
    }
    // The fifth closed bar is the first legal evaluation — and this stream
    // qualifies (baseline median 5s, run at 5s, ratio 100).
    CHECK(emitBar(strategy, tm, store, s, +1, seconds{5}) == Direction::LONG);
}

TEST_CASE("LONG on K consecutive fast up bars after a slow baseline",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    // Baseline: three 60s bars, directions deliberately mixed — only the
    // run bars' directions matter.
    CHECK_FALSE(emitBar(strategy, tm, store, s, +1, seconds{60}).has_value());
    CHECK_FALSE(emitBar(strategy, tm, store, s, -1, seconds{60}).has_value());
    CHECK_FALSE(emitBar(strategy, tm, store, s, +1, seconds{60}).has_value());
    // First run bar: fast, but n = 4 < 5 — still warming.
    CHECK_FALSE(emitBar(strategy, tm, store, s, +1, seconds{5}).has_value());
    // Second run bar: 12x faster than the 60s norm — LONG on this breach.
    CHECK(emitBar(strategy, tm, store, s, +1, seconds{5}) == Direction::LONG);
}

TEST_CASE("SHORT mirror on fast down bars", "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    CHECK_FALSE(emitBar(strategy, tm, store, s, -1, seconds{60}).has_value());
    CHECK_FALSE(emitBar(strategy, tm, store, s, +1, seconds{60}).has_value());
    CHECK_FALSE(emitBar(strategy, tm, store, s, -1, seconds{60}).has_value());
    CHECK_FALSE(emitBar(strategy, tm, store, s, -1, seconds{5}).has_value());
    CHECK(emitBar(strategy, tm, store, s, -1, seconds{5}) == Direction::SHORT);
}

TEST_CASE("fires exactly once per bar close, never on stale state",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    for (int i = 0; i < 3; ++i) {
        emitBar(strategy, tm, store, s, +1, seconds{60});
    }
    emitBar(strategy, tm, store, s, +1, seconds{5});
    REQUIRE(emitBar(strategy, tm, store, s, +1, seconds{5}) == Direction::LONG);

    // The successor's opening tick lands on the same just-closed series —
    // but the in-progress bar is back on top, so the gate is down again.
    s.price += 10;
    const PriceData openingTick(s.price, s.price - 2, s.clock, s.symbol);
    CHECK_FALSE(step(strategy, tm, store, openingTick).has_value());

    // Its breach re-evaluates freshly (the run extended): a NEW signal, one
    // per bar close — never a replay of the old one mid-bar.
    s.price += 10;
    s.clock += seconds{5};
    const PriceData breachTick(s.price, s.price - 2, s.clock, s.symbol);
    CHECK(step(strategy, tm, store, breachTick) == Direction::LONG);
}

TEST_CASE("a slower-than-allowed bar anywhere in the run blocks entry",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;
    for (int i = 0; i < 3; ++i) {
        emitBar(strategy, tm, store, s, +1, seconds{60});
    }

    SECTION("the newest run bar is too slow") {
        emitBar(strategy, tm, store, s, +1, seconds{5});
        CHECK_FALSE(
            emitBar(strategy, tm, store, s, +1, seconds{61}).has_value());
    }
    SECTION("the earlier run bar is too slow") {
        emitBar(strategy, tm, store, s, +1, seconds{61});
        CHECK_FALSE(
            emitBar(strategy, tm, store, s, +1, seconds{5}).has_value());
    }
}

TEST_CASE("an against-direction bar inside the run blocks entry",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;
    for (int i = 0; i < 3; ++i) {
        emitBar(strategy, tm, store, s, +1, seconds{60});
    }

    // Both run bars are fast, but they disagree on direction.
    emitBar(strategy, tm, store, s, -1, seconds{5});
    CHECK_FALSE(emitBar(strategy, tm, store, s, +1, seconds{5}).has_value());
}

TEST_CASE("the speed-ratio boundary admits exactly <=", "[rangeVelocity]") {
    // Ratio 50 over a 60s median allows exactly 30s per run bar.
    TradeManager tm;

    SECTION("exactly at the allowance enters") {
        RangeVelocityStrategy strategy{makeConfig(2, 3, 50)};
        auto store = makeStore();
        SymbolStream s;
        for (int i = 0; i < 3; ++i) {
            emitBar(strategy, tm, store, s, +1, seconds{60});
        }
        emitBar(strategy, tm, store, s, +1, seconds{30});
        CHECK(emitBar(strategy, tm, store, s, +1, seconds{30}) ==
              Direction::LONG);
    }
    SECTION("one microsecond over does not") {
        RangeVelocityStrategy strategy{makeConfig(2, 3, 50)};
        auto store = makeStore();
        SymbolStream s;
        for (int i = 0; i < 3; ++i) {
            emitBar(strategy, tm, store, s, +1, seconds{60});
        }
        emitBar(strategy, tm, store, s, +1, seconds{30});
        CHECK_FALSE(emitBar(strategy, tm, store, s, +1,
                            seconds{30} + microseconds{1})
                        .has_value());
    }
}

TEST_CASE("session gaps: the median absorbs a baseline gap, a run gap "
          "refuses entry", "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    SECTION("a weekend bar in the baseline leaves the median at the norm") {
        emitBar(strategy, tm, store, s, +1, seconds{60});
        emitBar(strategy, tm, store, s, -1, hours{72});  // the gap bar
        emitBar(strategy, tm, store, s, +1, seconds{60});
        emitBar(strategy, tm, store, s, +1, seconds{5});
        // Median of {60s, 72h, 60s} is 60s — the gap never poisons it.
        CHECK(emitBar(strategy, tm, store, s, +1, seconds{5}) ==
              Direction::LONG);
    }
    SECTION("a run bar spanning the gap fails the speed test") {
        for (int i = 0; i < 3; ++i) {
            emitBar(strategy, tm, store, s, +1, seconds{60});
        }
        emitBar(strategy, tm, store, s, +1, seconds{5});
        // The first bars after a session gap can never chase it.
        CHECK_FALSE(
            emitBar(strategy, tm, store, s, +1, hours{72}).has_value());
    }
}

TEST_CASE("during() closes on an opposite run of E closed bars — speed "
          "never enters exits", "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    SECTION("LONG closed at the bid on the Eth against-bar's breach") {
        for (int i = 0; i < 3; ++i) {
            emitBar(strategy, tm, store, s, +1, seconds{60});
        }
        emitBar(strategy, tm, store, s, +1, seconds{5});
        REQUIRE(emitBar(strategy, tm, store, s, +1, seconds{5}) ==
                Direction::LONG);
        tm.openTrade(s.lastBreach, 1, Direction::LONG);

        // First against-bar: the last two closed are [up, down] — open.
        // Deliberately SLOW bars: exits carry no speed filter.
        emitBar(strategy, tm, store, s, -1, seconds{600});
        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));

        // Second against-bar completes the opposite run: closed at the bid.
        emitBar(strategy, tm, store, s, -1, seconds{600});
        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == s.lastBreach.bid);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }
    SECTION("SHORT mirror closes at the ask") {
        for (int i = 0; i < 3; ++i) {
            emitBar(strategy, tm, store, s, -1, seconds{60});
        }
        emitBar(strategy, tm, store, s, -1, seconds{5});
        REQUIRE(emitBar(strategy, tm, store, s, -1, seconds{5}) ==
                Direction::SHORT);
        tm.openTrade(s.lastBreach, 1, Direction::SHORT);

        emitBar(strategy, tm, store, s, +1, seconds{600});
        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        emitBar(strategy, tm, store, s, +1, seconds{600});
        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == s.lastBreach.ask);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }
}

TEST_CASE("the opposite run must be unbroken", "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    for (int i = 0; i < 3; ++i) {
        emitBar(strategy, tm, store, s, +1, seconds{60});
    }
    emitBar(strategy, tm, store, s, +1, seconds{5});
    REQUIRE(emitBar(strategy, tm, store, s, +1, seconds{5}) == Direction::LONG);
    tm.openTrade(s.lastBreach, 1, Direction::LONG);

    // down, up, down: never two consecutive against-bars — stays open the
    // whole way (the intervening opening ticks can't close either: the
    // just-closed gate is down on them).
    emitBar(strategy, tm, store, s, -1, seconds{60});
    emitBar(strategy, tm, store, s, +1, seconds{60});
    emitBar(strategy, tm, store, s, -1, seconds{60});
    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

// The time-cap tests drive during() directly: its exit path is independent
// of bar state (an empty store just skips the opposite-run leg), and trades
// are opened straight on the TradeManager — exactly how the live book seeds
// them.
TEST_CASE("RangeVelocityStrategy closes trades past the max duration via "
          "during()", "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig(2, 3, 100, 2, 60)};
    TradeManager tm;

    SECTION("LONG past the cap closes at the bid") {
        tm.openTrade(tickAt(seconds{0}, 110000), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050),
                        bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110048);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }
    SECTION("SHORT past the cap closes at the ask") {
        tm.openTrade(tickAt(seconds{0}, 110000), 1, Direction::SHORT);
        strategy.during(tickAt(minutes{60} + seconds{1}, 110050),
                        bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110050);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }
    SECTION("exactly at the cap stays open (strictly greater)") {
        tm.openTrade(tickAt(seconds{0}, 110000), 1, Direction::LONG);
        strategy.during(tickAt(minutes{60}, 110050), bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
    SECTION("another symbol's tick never closes it") {
        tm.openTrade(tickAt(seconds{0}, 110000), 1, Direction::LONG);
        strategy.during(tickAt(minutes{120}, 65030, "AUDUSD"),
                        bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
}

TEST_CASE("RangeVelocityStrategy max duration of zero disables the time cap",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;

    tm.openTrade(tickAt(seconds{0}, 110000), 1, Direction::LONG);
    strategy.during(tickAt(minutes{600}, 110050), bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("re-signals on the next fast close after a stop-out mid-run",
          "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream s;

    for (int i = 0; i < 3; ++i) {
        emitBar(strategy, tm, store, s, +1, seconds{60});
    }
    emitBar(strategy, tm, store, s, +1, seconds{5});
    REQUIRE(emitBar(strategy, tm, store, s, +1, seconds{5}) == Direction::LONG);
    tm.openTrade(s.lastBreach, 1, Direction::LONG);

    // Simulated stop-out: the broker/SL closed it, not the strategy.
    tm.closeTrade("EURUSD", s.lastBreach.bid, s.lastBreach);
    REQUIRE_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));

    // The run is still alive and fast — the next bar close re-signals
    // (one-trade-per-symbol gating is the run loop's job, not decide()'s).
    CHECK(emitBar(strategy, tm, store, s, +1, seconds{5}) == Direction::LONG);
}

TEST_CASE("per-symbol streams signal independently", "[rangeVelocity]") {
    RangeVelocityStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore();
    SymbolStream eur;
    SymbolStream aud{.symbol = "AUDUSD", .price = 65000};

    // Interleave the two streams; each keeps its own bar history.
    for (int i = 0; i < 3; ++i) {
        emitBar(strategy, tm, store, eur, +1, seconds{60});
        emitBar(strategy, tm, store, aud, -1, seconds{60});
    }
    emitBar(strategy, tm, store, eur, +1, seconds{5});
    emitBar(strategy, tm, store, aud, -1, seconds{5});
    CHECK(emitBar(strategy, tm, store, eur, +1, seconds{5}) == Direction::LONG);
    CHECK(emitBar(strategy, tm, store, aud, -1, seconds{5}) ==
          Direction::SHORT);
}

TEST_CASE("StrategyVariables round-trips RANGE_VELOCITY_VARIABLES through "
          "JSON", "[rangeVelocity]") {
    SECTION("a present group survives the round trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.RANGE_VELOCITY_VARIABLES = tradingDefinitions::RangeVelocityVariables{
            .RUN_BARS = 3,
            .SPEED_LOOKBACK_BARS = 32,
            .SPEED_RATIO_PERCENT = 60,
            .EXIT_RUN_BARS = 2,
            .MAX_TRADE_DURATION_MINUTES = 240};

        const nlohmann::json j = vars;
        const auto parsed = j.get<tradingDefinitions::StrategyVariables>();
        REQUIRE(parsed.RANGE_VELOCITY_VARIABLES.has_value());
        CHECK(parsed.RANGE_VELOCITY_VARIABLES->RUN_BARS == 3);
        CHECK(parsed.RANGE_VELOCITY_VARIABLES->SPEED_LOOKBACK_BARS == 32);
        CHECK(parsed.RANGE_VELOCITY_VARIABLES->SPEED_RATIO_PERCENT == 60);
        CHECK(parsed.RANGE_VELOCITY_VARIABLES->EXIT_RUN_BARS == 2);
        CHECK(parsed.RANGE_VELOCITY_VARIABLES->MAX_TRADE_DURATION_MINUTES == 240);
    }
    SECTION("an absent field parses with the zero default (WITH_DEFAULT)") {
        const nlohmann::json j = {{"RUN_BARS", 3},
                                  {"SPEED_LOOKBACK_BARS", 32},
                                  {"SPEED_RATIO_PERCENT", 60},
                                  {"EXIT_RUN_BARS", 2}};
        const auto rv = j.get<tradingDefinitions::RangeVelocityVariables>();
        CHECK(rv.RUN_BARS == 3);
        CHECK(rv.MAX_TRADE_DURATION_MINUTES == 0);
    }
    SECTION("an absent group serialises as null and parses back absent") {
        const nlohmann::json j = tradingDefinitions::StrategyVariables{};
        CHECK(j.at("RANGE_VELOCITY_VARIABLES").is_null());
        const auto parsed = j.get<tradingDefinitions::StrategyVariables>();
        CHECK_FALSE(parsed.RANGE_VELOCITY_VARIABLES.has_value());
    }
}
