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

import liquiditySweepReversalStrategy;
import barStore;  // bars::BarStore — the strategy reads bars from it
import priceData;
import trade;
import tradeManager;

namespace {

using std::chrono::minutes;
using std::chrono::seconds;

const std::chrono::system_clock::time_point kBase =
    std::chrono::sys_days{std::chrono::year{2026} / 1 / 5};

// Window derived exactly like the sweep mapper — the ctor minimum:
// max(LOOKBACK_BARS + PIVOT_BARS + 1, VALID_BARS + 11).
int deriveOhlcCount(int pivotBars, int lookbackBars, int validBars) {
    return std::max(lookbackBars + pivotBars + 1, validBars + 11);
}

tradingDefinitions::StrategyConfig makeConfig(int pivotBars = 2,
                                              int lookbackBars = 8,
                                              int minSweepPips = 2,
                                              int displacementAtrTenths = 0,
                                              int validBars = 2,
                                              int ohlcMinutes = 15,
                                              int maxTradeDurationMinutes = 0) {
    tradingDefinitions::StrategyConfig config;
    config.UUID = "test-liquidity-sweep";
    config.TRADING_VARIABLES.STRATEGY = "LiquiditySweepReversalStrategy";
    config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR = 10;
    config.TRADING_VARIABLES.TRADING_SIZE = 1;
    config.OHLC_VARIABLES = {
        tradingDefinitions::OHLCVariables{
            .OHLC_COUNT = deriveOhlcCount(pivotBars, lookbackBars, validBars),
            .OHLC_MINUTES = ohlcMinutes},
    };
    config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES =
        tradingDefinitions::LiquiditySweepReversalVariables{
            .PIVOT_BARS = pivotBars,
            .LOOKBACK_BARS = lookbackBars,
            .MIN_SWEEP_PIPS = minSweepPips,
            .DISPLACEMENT_ATR_TENTHS = displacementAtrTenths,
            .VALID_BARS = validBars,
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
// management hook.
std::optional<Direction> step(LiquiditySweepReversalStrategy& strategy,
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

// Four asks inside one 15m bar. Unlike the session strategies there is no
// clock gate to guarantee silence while feeding, and live setups DO fire on
// feed ticks once the window is warm — so feedBar deliberately ignores
// signals; the tests assert on explicit decision ticks (and the no-lookahead
// test steps the sweep bar tick by tick itself).
void feedBar(LiquiditySweepReversalStrategy& strategy, TradeManager& tm,
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
        step(strategy, tm, store,
             tickAt(base, offset + tickOffset, ask, ask - 10, symbol));
    }
}

// Feeds bars every 20 minutes (first-tick-anchored 15m bars roll on the next
// feed), starting at `startOffset`.
template <std::size_t N>
void feedBars(LiquiditySweepReversalStrategy& strategy, TradeManager& tm,
              bars::BarStore& store, std::chrono::system_clock::time_point base,
              minutes startOffset, const std::array<BarShape, N>& bars) {
    for (std::size_t i = 0; i < N; ++i) {
        feedBar(strategy, tm, store, base,
                startOffset + minutes{20} * static_cast<int>(i), bars[i]);
    }
}

// Canonical swept-high fixture: two pad bars (keep the warm-up gate ahead of
// the interesting bars), then a gentle up-drift whose bar [6] is a strict
// 2-wing swing high at 110100. EURUSD scale 10, MIN_SWEEP_PIPS 2 -> the
// sweep must poke >= 20 points beyond 110100.
constexpr std::array<BarShape, 12> kSweptHighFixture{{
    {110000, 110020, 109980, 110010},  // pad
    {110000, 110020, 109980, 110010},  // pad
    {110000, 110020, 109980, 110010},  // [0]
    {110010, 110030, 109990, 110020},  // [1]
    {110020, 110040, 110000, 110030},  // [2]
    {110030, 110050, 110010, 110040},  // [3]
    {110040, 110060, 110020, 110050},  // [4]
    {110050, 110070, 110030, 110060},  // [5]
    {110060, 110100, 110040, 110080},  // [6] swing high: level 110100
    {110080, 110090, 110050, 110070},  // [7]
    {110070, 110080, 110040, 110060},  // [8]
    {110060, 110070, 110030, 110050},  // [9]
}};

// The rejection sweep: pokes 30 points through 110100, closes back inside
// with a 40-point bearish body (open 110120 -> close 110080).
constexpr BarShape kRejectionSweep{110120, 110130, 110040, 110080};
// A benign tail bar that closes the sweep bar without touching the level.
constexpr BarShape kBenignTail{110080, 110090, 110030, 110060};

// Offsets: fixture bar i sits at 20 x i minutes; the next free slot follows.
constexpr minutes kSweepOffset{240};   // right after the 12 fixture bars
constexpr minutes kTailOffset{260};
constexpr minutes kDecisionOffset{280};

}  // namespace

TEST_CASE("LiquiditySweepReversalStrategy fades a swept swing high",
          "[liquiditySweepReversal]") {
    LiquiditySweepReversalStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);

    SECTION("rejection deep enough: SHORT while the bid is back below") {
        feedBar(strategy, tm, store, kBase, kSweepOffset, kRejectionSweep);
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK(step(strategy, tm, store,
                   tickAt(kBase, kDecisionOffset, 110090, 110080)) ==
              Direction::SHORT);
    }

    SECTION("exactly MIN_SWEEP_PIPS deep still rejects") {
        feedBar(strategy, tm, store, kBase, kSweepOffset,
                {110110, 110120, 110040, 110080});
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK(step(strategy, tm, store,
                   tickAt(kBase, kDecisionOffset, 110090, 110080)) ==
              Direction::SHORT);
    }

    SECTION("fill-side gate: no SHORT while the bid still sits at the level") {
        feedBar(strategy, tm, store, kBase, kSweepOffset, kRejectionSweep);
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset, 110120, 110110))
                        .has_value());
    }
}

TEST_CASE("LiquiditySweepReversalStrategy never trades an unconfirmed rejection",
          "[liquiditySweepReversal]") {
    // No-lookahead: while the sweep bar is still IN PROGRESS its wick beyond
    // the level is visible in the store, but the rejection isn't a closed
    // fact yet — every tick of the bar must stay silent, including the ones
    // where the bid is already back below the level. The tick that ROLLS the
    // bar closed is the first one allowed to trade it.
    LiquiditySweepReversalStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);

    const std::array<std::pair<seconds, std::int32_t>, 4> sweepTicks{{
        {seconds{0}, kRejectionSweep.o},
        {seconds{15}, kRejectionSweep.h},
        {seconds{30}, kRejectionSweep.l},
        {seconds{45}, kRejectionSweep.c},
    }};
    for (const auto& [tickOffset, ask] : sweepTicks) {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kSweepOffset + tickOffset, ask, ask - 10))
                        .has_value());
    }

    // 20 minutes on, the first tick of the next bar closes the sweep bar —
    // the rejection now exists and this same tick may trade it.
    CHECK(step(strategy, tm, store,
               tickAt(kBase, kTailOffset, 110080, 110070)) == Direction::SHORT);
}

TEST_CASE("LiquiditySweepReversalStrategy lets the first touch consume the level",
          "[liquiditySweepReversal]") {
    LiquiditySweepReversalStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);

    SECTION("a breakout close kills the level; a later poke never revives it") {
        // First touch closes ABOVE the level -> breakout, level dead.
        feedBar(strategy, tm, store, kBase, kSweepOffset,
                {110120, 110130, 110090, 110110});
        // A textbook rejection shape follows — but it is the SECOND touch.
        feedBar(strategy, tm, store, kBase, kTailOffset,
                {110105, 110125, 110060, 110080});
        feedBar(strategy, tm, store, kBase, kDecisionOffset,
                {110080, 110090, 110050, 110070});
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset + minutes{20}, 110090,
                                110080))
                        .has_value());
    }

    SECTION("a shallow tap kills the level; a later deep sweep never revives it") {
        // First touch pokes only 10 points (< MIN_SWEEP_PIPS x 10) -> tap.
        feedBar(strategy, tm, store, kBase, kSweepOffset,
                {110105, 110110, 110040, 110080});
        // A deep, well-formed sweep follows — but the level is already spent.
        feedBar(strategy, tm, store, kBase, kTailOffset, kRejectionSweep);
        feedBar(strategy, tm, store, kBase, kDecisionOffset, kBenignTail);
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset + minutes{20}, 110090,
                                110080))
                        .has_value());
    }
}

TEST_CASE("LiquiditySweepReversalStrategy gates the rejection on displacement",
          "[liquiditySweepReversal]") {
    // ATR(10) frozen at the rejection bar is 47 points for this fixture; the
    // rejection body is 40. tenths = 10 demands a full ATR -> refused;
    // tenths = 5 demands half -> passes.
    SECTION("body smaller than the demanded displacement: no trade") {
        LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 10)};
        TradeManager tm;
        auto store = makeStore(makeConfig(2, 8, 2, 10));
        feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);
        feedBar(strategy, tm, store, kBase, kSweepOffset, kRejectionSweep);
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset, 110090, 110080))
                        .has_value());
    }

    SECTION("body clearing the demanded displacement: SHORT") {
        LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 5)};
        TradeManager tm;
        auto store = makeStore(makeConfig(2, 8, 2, 5));
        feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);
        feedBar(strategy, tm, store, kBase, kSweepOffset, kRejectionSweep);
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK(step(strategy, tm, store,
                   tickAt(kBase, kDecisionOffset, 110090, 110080)) ==
              Direction::SHORT);
    }

    SECTION("a wrong-way body is no displacement, whatever its size") {
        // Same sweep depth but a BULLISH body off a swept high.
        LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 5)};
        TradeManager tm;
        auto store = makeStore(makeConfig(2, 8, 2, 5));
        feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);
        feedBar(strategy, tm, store, kBase, kSweepOffset,
                {110040, 110130, 110035, 110080});
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset, 110090, 110080))
                        .has_value());
    }

    SECTION("tenths of zero disables the gate, body direction included") {
        LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 0)};
        TradeManager tm;
        auto store = makeStore(makeConfig(2, 8, 2, 0));
        feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);
        feedBar(strategy, tm, store, kBase, kSweepOffset,
                {110040, 110130, 110035, 110080});
        feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
        CHECK(step(strategy, tm, store,
                   tickAt(kBase, kDecisionOffset, 110090, 110080)) ==
              Direction::SHORT);
    }
}

TEST_CASE("LiquiditySweepReversalStrategy ages a rejection out after VALID_BARS",
          "[liquiditySweepReversal]") {
    LiquiditySweepReversalStrategy strategy{makeConfig()};  // VALID_BARS = 2
    TradeManager tm;
    auto store = makeStore(makeConfig());
    feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);
    feedBar(strategy, tm, store, kBase, kSweepOffset, kRejectionSweep);
    feedBar(strategy, tm, store, kBase, kTailOffset, kBenignTail);
    // A second benign bar pushes the rejection to three closed bars back —
    // past the freshness window.
    feedBar(strategy, tm, store, kBase, kDecisionOffset,
            {110060, 110070, 110020, 110050});

    CHECK_FALSE(step(strategy, tm, store,
                     tickAt(kBase, kDecisionOffset + minutes{20}, 110090,
                            110080))
                    .has_value());
}

TEST_CASE("LiquiditySweepReversalStrategy drops a rejection once price closes back beyond",
          "[liquiditySweepReversal]") {
    // VALID_BARS = 3 so the extra bar cannot age the setup out — what kills
    // it (or not) is that bar's CLOSE relative to the level.
    LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 0, 3)};
    TradeManager tm;
    auto store = makeStore(makeConfig(2, 8, 2, 0, 3));
    feedBars(strategy, tm, store, kBase, minutes{0}, kSweptHighFixture);
    feedBar(strategy, tm, store, kBase, kSweepOffset, kRejectionSweep);

    SECTION("a later close above the level invalidates the setup") {
        feedBar(strategy, tm, store, kBase, kTailOffset,
                {110090, 110120, 110080, 110110});
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset, 110090, 110080))
                        .has_value());
    }

    SECTION("control: the same bar closing back inside leaves it tradeable") {
        feedBar(strategy, tm, store, kBase, kTailOffset,
                {110090, 110120, 110080, 110090});
        CHECK(step(strategy, tm, store,
                   tickAt(kBase, kDecisionOffset, 110090, 110080)) ==
              Direction::SHORT);
    }
}

TEST_CASE("LiquiditySweepReversalStrategy fades a swept swing low with a LONG",
          "[liquiditySweepReversal]") {
    // Exact mirror of the swept-high fixture: down-drift, swing low 109900 at
    // bar [6], bullish rejection sweeping 30 points below.
    LiquiditySweepReversalStrategy strategy{makeConfig()};
    TradeManager tm;
    auto store = makeStore(makeConfig());
    const std::array<BarShape, 12> sweptLowFixture{{
        {110000, 110020, 109980, 109990},  // pad
        {110000, 110020, 109980, 109990},  // pad
        {110000, 110020, 109980, 109990},  // [0]
        {109990, 110010, 109970, 109980},  // [1]
        {109980, 110000, 109960, 109970},  // [2]
        {109970, 109990, 109950, 109960},  // [3]
        {109960, 109980, 109940, 109950},  // [4]
        {109950, 109970, 109930, 109940},  // [5]
        {109940, 109960, 109900, 109920},  // [6] swing low: level 109900
        {109920, 109950, 109910, 109930},  // [7]
        {109930, 109960, 109920, 109940},  // [8]
        {109940, 109970, 109930, 109950},  // [9]
    }};
    feedBars(strategy, tm, store, kBase, minutes{0}, sweptLowFixture);
    feedBar(strategy, tm, store, kBase, kSweepOffset,
            {109880, 109960, 109870, 109920});
    feedBar(strategy, tm, store, kBase, kTailOffset,
            {109920, 109970, 109910, 109940});

    SECTION("LONG while the ask is back above the level") {
        CHECK(step(strategy, tm, store,
                   tickAt(kBase, kDecisionOffset, 109920, 109910)) ==
              Direction::LONG);
    }

    SECTION("fill-side gate: no LONG while the ask still sits at the level") {
        CHECK_FALSE(step(strategy, tm, store,
                         tickAt(kBase, kDecisionOffset, 109900, 109890))
                        .has_value());
    }
}

TEST_CASE("LiquiditySweepReversalStrategy refuses an outside bar rejecting both ways",
          "[liquiditySweepReversal]") {
    // One bar sweeps a swing high AND a swing low and closes inside both —
    // the direction is ambiguous, so the strategy must stand aside even
    // though either side alone would have been tradeable. VALID_BARS = 3
    // keeps the shared rejection fresh at the decision tick.
    LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 0, 3)};
    TradeManager tm;
    auto store = makeStore(makeConfig(2, 8, 2, 0, 3));
    const std::array<BarShape, 12> rangeFixture{{
        {110000, 110040, 109960, 110000},  // pad
        {110000, 110040, 109960, 110000},  // pad
        {110000, 110050, 109970, 110010},  // [0]
        {110010, 110060, 109960, 110000},  // [1]
        {110000, 110070, 109950, 110010},  // [2]
        {110010, 110100, 109940, 110020},  // [3] swing high: level 110100
        {110020, 110080, 109930, 110000},  // [4]
        {110000, 110060, 109900, 109990},  // [5] swing low: level 109900
        {109990, 110050, 109940, 110000},  // [6]
        {110000, 110040, 109950, 110010},  // [7]
        {110010, 110130, 109870, 110000},  // [8] outside bar: sweeps both
        {110000, 110040, 109950, 110000},  // [9]
    }};
    feedBars(strategy, tm, store, kBase, minutes{0}, rangeFixture);
    feedBar(strategy, tm, store, kBase, kSweepOffset,
            {110000, 110050, 109940, 110010});

    CHECK_FALSE(step(strategy, tm, store,
                     tickAt(kBase, kTailOffset, 110010, 110000))
                    .has_value());
}

// The time-cap tests drive during() directly: its exit path is independent of
// bar state (no warm-up needed), and trades are opened straight on the
// TradeManager — same harness as the NyOpenRangeBreakoutStrategy cap tests.
TEST_CASE("LiquiditySweepReversalStrategy closes trades past the max duration via during()",
          "[liquiditySweepReversal]") {
    LiquiditySweepReversalStrategy strategy{makeConfig(2, 8, 2, 0, 2, 15, 60)};
    TradeManager tm;

    SECTION("LONG past the cap closes at the bid") {
        tm.openTrade(tickAt(kBase, seconds{0}, 110000, 109998), 1,
                     Direction::LONG);
        strategy.during(
            tickAt(kBase, minutes{60} + seconds{1}, 110050, 110040),
            bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110040);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("SHORT past the cap closes at the ask") {
        tm.openTrade(tickAt(kBase, seconds{0}, 110000, 109998), 1,
                     Direction::SHORT);
        strategy.during(
            tickAt(kBase, minutes{60} + seconds{1}, 110050, 110040),
            bars::BarStore{}, tm);

        REQUIRE(tm.getClosedTrades().size() == 1);
        CHECK(tm.getClosedTrades().front().closePrice == 110050);
        CHECK_FALSE(tm.hasActiveTradeForSymbol("EURUSD"));
    }

    SECTION("exactly at the cap stays open (strictly greater)") {
        tm.openTrade(tickAt(kBase, seconds{0}, 110000, 109998), 1,
                     Direction::LONG);
        strategy.during(tickAt(kBase, minutes{60}, 110050, 110040),
                        bars::BarStore{}, tm);

        CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
        CHECK(tm.getClosedTrades().empty());
    }
}

// With the cap at 0 (the pre-cap winner-config default) exits stay owned by
// Operations via SL/TP — during() must not touch open positions.
TEST_CASE("LiquiditySweepReversalStrategy max duration of zero disables the exit",
          "[liquiditySweepReversal]") {
    LiquiditySweepReversalStrategy strategy{makeConfig()};
    TradeManager tm;

    tm.openTrade(tickAt(kBase, seconds{0}, 110000, 109990), 1,
                 Direction::LONG);
    strategy.during(tickAt(kBase, minutes{600}, 110050, 110040),
                    bars::BarStore{}, tm);

    CHECK(tm.hasActiveTradeForSymbol("EURUSD"));
    CHECK(tm.getClosedTrades().empty());
}

TEST_CASE("LiquiditySweepReversalStrategy rejects malformed configuration",
          "[liquiditySweepReversal]") {
    SECTION("no OHLC timeframe") {
        auto config = makeConfig();
        config.OHLC_VARIABLES.clear();
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("missing LIQUIDITY_SWEEP_REVERSAL_VARIABLES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES =
            std::nullopt;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("PIVOT_BARS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->PIVOT_BARS = 0;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("lookback too small to hold a pivot plus its wing") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->LOOKBACK_BARS = 2;  // PIVOT_BARS is 2 -> needs >= 3
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("negative MIN_SWEEP_PIPS") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->MIN_SWEEP_PIPS = -1;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("negative DISPLACEMENT_ATR_TENTHS") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->DISPLACEMENT_ATR_TENTHS = -1;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("VALID_BARS below 1") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->VALID_BARS = 0;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("VALID_BARS beyond the lookback") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->VALID_BARS = 9;  // LOOKBACK_BARS is 8
        // Keep the window minimum satisfied so the ordering rule is what
        // throws.
        config.OHLC_VARIABLES[0].OHLC_COUNT = 40;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("negative MAX_TRADE_DURATION_MINUTES") {
        auto config = makeConfig();
        config.STRATEGY_VARIABLES.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
            ->MAX_TRADE_DURATION_MINUTES = -1;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("window below the scan + warm-ATR minimum") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_COUNT -= 1;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }

    SECTION("OHLC_MINUTES below 1") {
        auto config = makeConfig();
        config.OHLC_VARIABLES[0].OHLC_MINUTES = 0;
        CHECK_THROWS_AS(LiquiditySweepReversalStrategy{config},
                        std::invalid_argument);
    }
}

TEST_CASE("StrategyVariables round-trips LIQUIDITY_SWEEP_REVERSAL_VARIABLES through JSON",
          "[liquiditySweepReversal]") {
    SECTION("present group survives the round-trip") {
        tradingDefinitions::StrategyVariables vars;
        vars.LIQUIDITY_SWEEP_REVERSAL_VARIABLES =
            tradingDefinitions::LiquiditySweepReversalVariables{
                .PIVOT_BARS = 3,
                .LOOKBACK_BARS = 48,
                .MIN_SWEEP_PIPS = 5,
                .DISPLACEMENT_ATR_TENTHS = 10,
                .VALID_BARS = 4,
                .MAX_TRADE_DURATION_MINUTES = 45};

        const nlohmann::json j = vars;
        const auto back = j.get<tradingDefinitions::StrategyVariables>();

        REQUIRE(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES.has_value());
        CHECK(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->PIVOT_BARS == 3);
        CHECK(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->LOOKBACK_BARS == 48);
        CHECK(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->MIN_SWEEP_PIPS == 5);
        CHECK(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->DISPLACEMENT_ATR_TENTHS ==
              10);
        CHECK(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->VALID_BARS == 4);
        CHECK(back.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
                  ->MAX_TRADE_DURATION_MINUTES == 45);
    }

    SECTION("absent DISPLACEMENT_ATR_TENTHS parses as disabled") {
        // Models a winner config persisted before the field existed: the
        // WITH_DEFAULT codec must fall back to 0, not throw.
        const auto vars =
            nlohmann::json::parse(
                R"({"LIQUIDITY_SWEEP_REVERSAL_VARIABLES":{"PIVOT_BARS":2,)"
                R"("LOOKBACK_BARS":48,"MIN_SWEEP_PIPS":2,"VALID_BARS":2}})")
                .get<tradingDefinitions::StrategyVariables>();

        REQUIRE(vars.LIQUIDITY_SWEEP_REVERSAL_VARIABLES.has_value());
        CHECK(vars.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->LOOKBACK_BARS == 48);
        CHECK(vars.LIQUIDITY_SWEEP_REVERSAL_VARIABLES->DISPLACEMENT_ATR_TENTHS ==
              0);
        // Same doctrine for the cap: absent = 0 = disabled, the pre-cap
        // behaviour those persisted winners were scored with.
        CHECK(vars.LIQUIDITY_SWEEP_REVERSAL_VARIABLES
                  ->MAX_TRADE_DURATION_MINUTES == 0);
    }

    SECTION("absent group serialises as null and stays absent") {
        const tradingDefinitions::StrategyVariables vars;
        const nlohmann::json j = vars;

        CHECK(j.at("LIQUIDITY_SWEEP_REVERSAL_VARIABLES").is_null());
        CHECK_FALSE(j.get<tradingDefinitions::StrategyVariables>()
                        .LIQUIDITY_SWEEP_REVERSAL_VARIABLES.has_value());
    }
}
