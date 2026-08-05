// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// selectTopWinners is the pure half of liveWinners (no Elasticsearch): these
// tests drive it with synthetic _search response bodies, pinning the
// selection machinery — filtering, symbol splitting, per-group ranking,
// behavioural-clone dedup, the behavioural diversity gate (trade count /
// PnL / long-short mix, with the config-space gate as the legacy fallback
// for documents predating results.tradesClosed) and malformed-hit
// tolerance — not any currently-configured sweep values. Fixtures that omit
// tradesClosed parse to the -1 fallback and thus exercise the LEGACY
// diversity path; behavioural-gate tests must set it. buildWinnersQueryBody
// is pinned too (the strategy .keyword term and the analyzed-SYMBOLS match
// are both silent-failure-shaped if they regress).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "shared/tradingDefinitions/config/runConfiguration.hpp"
#include "shared/tradingDefinitions/strategyConfig.hpp"

import liveWinners;

namespace {

constexpr std::array<std::string_view, 2> kActive{"RandomStrategy",
                                                  "OhlcBreakoutStrategy"};

// Optional knobs for the dedup/diversity machinery. Defaults reproduce the
// pre-existing fixture shape: empty OHLC_VARIABLES and a results object
// carrying only performanceScore — which doubles as the "old document" shape
// for the fallback-key tests.
struct HitExtras {
    std::vector<tradingDefinitions::OHLCVariables> ohlc{};
    // Emitted only when non-empty, so the default fixture keeps the
    // pre-range document shape — every test without it doubles as the
    // backward-compat pin for RANGE_VARIABLES-less winners.
    std::vector<tradingDefinitions::RangeBarVariables> range{};
    std::optional<double> finalPnl{};
    std::optional<long long> tradesClosed{};
    // Emitted only when set — every fixture without them doubles as the
    // mid-vintage pin (docs carrying tradesClosed but predating the
    // opened counts), where the direction axis must stand down.
    std::optional<long long> openedLong{};
    std::optional<long long> openedShort{};
};

// One winners-index hit as the reporting path writes it. The ES documents
// carry TRADING_VARIABLES re-typed as JSON numbers (reportConfigJson); the
// Redis sweep payloads carry them string-encoded — both shapes must parse, so
// the fixture can produce either.
nlohmann::json makeHit(const std::string& runId, const std::string& symbols,
                       const std::string& strategyName, const std::string& uuid,
                       const double score, const bool numericVars = true,
                       const HitExtras& extras = {}) {
    nlohmann::json vars{{"STRATEGY", strategyName}};
    if (numericVars) {
        vars["STOP_DISTANCE_IN_ATR"] = 25;
        vars["LIMIT_DISTANCE_IN_ATR"] = 50;
        vars["TRADING_SIZE"] = 3;
    } else {
        vars["STOP_DISTANCE_IN_ATR"] = "25";
        vars["LIMIT_DISTANCE_IN_ATR"] = "50";
        vars["TRADING_SIZE"] = "3";
    }
    nlohmann::json results{{"performanceScore", score}};
    if (extras.finalPnl) {
        results["finalPnl"] = *extras.finalPnl;
    }
    if (extras.tradesClosed) {
        results["tradesClosed"] = *extras.tradesClosed;
    }
    if (extras.openedLong) {
        results["openedLong"] = *extras.openedLong;
    }
    if (extras.openedShort) {
        results["openedShort"] = *extras.openedShort;
    }
    nlohmann::json hit{{"_source",
        {{"RUN_ID", runId},
         {"config",
          {{"SYMBOLS", symbols},
           {"STRATEGY",
            {{"UUID", uuid},
             {"TRADING_VARIABLES", vars},
             {"OHLC_VARIABLES", nlohmann::json(extras.ohlc)},
             {"STRATEGY_VARIABLES", nlohmann::json::object()}}}}},
         {"results", results}}}};
    if (!extras.range.empty()) {
        hit["_source"]["config"]["STRATEGY"]["RANGE_VARIABLES"] = extras.range;
    }
    return hit;
}

std::string makeResponse(const std::vector<nlohmann::json>& hits) {
    return nlohmann::json{
        {"hits", {{"total", {{"value", hits.size()}}}, {"hits", hits}}}}
        .dump();
}

tradingDefinitions::OHLCVariables series(const int count, const int minutes) {
    return {.OHLC_COUNT = count, .OHLC_MINUTES = minutes};
}

}  // namespace

TEST_CASE("selectTopWinners keeps the top N per (symbol, strategy) by score",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "uuid-40", 40.0),
        makeHit("r2", "EURUSD", "RandomStrategy", "uuid-42", 42.0),
        makeHit("r3", "EURUSD", "RandomStrategy", "uuid-39", 39.0),
        makeHit("r4", "EURUSD", "RandomStrategy", "uuid-41", 41.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].config.UUID == "uuid-42");
    CHECK(winners[1].config.UUID == "uuid-41");
    CHECK(winners[0].performanceScore == 42.0);
    CHECK(winners[0].symbol == "EURUSD");
    CHECK(winners[0].strategyName == "RandomStrategy");
    CHECK(winners[0].runId == "r2");
}

TEST_CASE("selectTopWinners ranks strategies independently per group",
          "[liveWinners]") {
    // Two strategies on one symbol, one strategy on another: each group keeps
    // its own top 2, so a dominant strategy cannot crowd out the others.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-r1", 50.0),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-r2", 49.0),
        makeHit("r3", "EURUSD", "RandomStrategy", "u-r3", 48.0),
        makeHit("r4", "EURUSD", "OhlcBreakoutStrategy", "u-o1", 11.0),
        makeHit("r5", "USDJPY", "OhlcBreakoutStrategy", "u-o2", 12.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 4);
    // Output is ordered by symbol then strategy name (std::map iteration).
    CHECK(winners[0].config.UUID == "u-o1");  // EURUSD / OhlcBreakout
    CHECK(winners[1].config.UUID == "u-r1");  // EURUSD / Random, top score
    CHECK(winners[2].config.UUID == "u-r2");  // EURUSD / Random, second
    CHECK(winners[3].config.UUID == "u-o2");  // USDJPY / OhlcBreakout
}

TEST_CASE("selectTopWinners drops strategies not in the active list",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RetiredStrategy", "u-x", 99.0),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-r", 20.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 1);
    CHECK(winners[0].config.UUID == "u-r");
}

TEST_CASE("selectTopWinners splits a multi-symbol run into one winner per symbol",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD,USDJPY", "RandomStrategy", "u-multi", 30.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].symbol == "EURUSD");
    CHECK(winners[1].symbol == "USDJPY");
    CHECK(winners[0].config.UUID == "u-multi");
    CHECK(winners[1].config.UUID == "u-multi");
}

TEST_CASE("selectTopWinners trims whitespace and drops empty fields in SYMBOLS",
          "[liveWinners]") {
    // Historical documents can carry hand-written symbol groups; an untrimmed
    // " USDJPY" would cache a worker no decoded tick could ever route to.
    const std::string body = makeResponse({
        makeHit("r1", " EURUSD , USDJPY ,", "RandomStrategy", "u-ws", 30.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].symbol == "EURUSD");
    CHECK(winners[1].symbol == "USDJPY");
}

TEST_CASE("selectTopWinners skips a malformed hit and keeps the rest",
          "[liveWinners]") {
    nlohmann::json broken = makeHit("r1", "EURUSD", "RandomStrategy", "u-b", 44.0);
    broken["_source"]["config"].erase("STRATEGY");

    const std::string body = makeResponse({
        broken,
        makeHit("r2", "EURUSD", "RandomStrategy", "u-ok", 33.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 1);
    CHECK(winners[0].config.UUID == "u-ok");
}

TEST_CASE("selectTopWinners parses numeric and string TRADING_VARIABLES",
          "[liveWinners]") {
    // numericVars=true is the reporting-path document shape
    // (reportConfigJson re-types the pip/size fields as JSON numbers);
    // numericVars=false is the shared string-encoded convention.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-num", 20.0, true),
        makeHit("r2", "USDJPY", "RandomStrategy", "u-str", 21.0, false),
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 2);
    for (const auto& winner : winners) {
        CHECK(winner.config.TRADING_VARIABLES.STOP_DISTANCE_IN_ATR == 25);
        CHECK(winner.config.TRADING_VARIABLES.LIMIT_DISTANCE_IN_ATR == 50);
        CHECK(winner.config.TRADING_VARIABLES.TRADING_SIZE == 3);
        CHECK(winner.config.TRADING_VARIABLES.STRATEGY == "RandomStrategy");
    }
}

TEST_CASE("selectTopWinners carries the run-level risk caps and defaults "
          "documents that predate them",
          "[liveWinners]") {
    nlohmann::json capped =
        makeHit("r1", "EURUSD", "RandomStrategy", "u-cap", 30.0);
    capped["_source"]["config"]["MAX_OPEN_TRADES"] = 4;
    capped["_source"]["config"]["MAX_TRADES_PER_MINUTE"] = 7;
    capped["_source"]["config"]["PEAK_HOURS_ONLY"] = true;

    const std::string body = makeResponse({
        capped,
        makeHit("r2", "USDJPY", "RandomStrategy", "u-old", 30.0),  // no caps
    });

    const auto winners = live::selectTopWinners(body, kActive, 2);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].config.UUID == "u-cap");
    CHECK(winners[0].maxOpenTrades == 4);
    CHECK(winners[0].maxTradesPerMinute == 7);
    CHECK(winners[0].peakHoursOnly == true);

    // Pre-cap documents fall back to the same defaults the backtest itself
    // would have applied — RunConfiguration's, not literals repeated here.
    const tradingDefinitions::RunConfiguration defaults{};
    CHECK(winners[1].config.UUID == "u-old");
    CHECK(winners[1].maxOpenTrades == defaults.MAX_OPEN_TRADES);
    CHECK(winners[1].maxTradesPerMinute == defaults.MAX_TRADES_PER_MINUTE);
    CHECK(winners[1].peakHoursOnly == defaults.PEAK_HOURS_ONLY);
}

TEST_CASE("selectTopWinners returns empty on garbage or empty responses",
          "[liveWinners]") {
    CHECK(live::selectTopWinners("not json at all", kActive, 2).empty());
    CHECK(live::selectTopWinners("{}", kActive, 2).empty());
    CHECK(live::selectTopWinners(makeResponse({}), kActive, 2).empty());
}

TEST_CASE("selectTopWinners survives 2xx bodies that are not ES envelopes",
          "[liveWinners]") {
    // A proxy in front of Elasticsearch can return 200 with its own JSON
    // (error envelopes, arrays, nulls). These parse fine but are not
    // _search-shaped; they must take the empty-return path, not throw an
    // uncaught type_error out of live startup.
    CHECK(live::selectTopWinners("[1, 2, 3]", kActive, 2).empty());
    CHECK(live::selectTopWinners("\"gateway timeout\"", kActive, 2).empty());
    CHECK(live::selectTopWinners("null", kActive, 2).empty());
    CHECK(live::selectTopWinners(R"({"hits": null})", kActive, 2).empty());
    CHECK(live::selectTopWinners(R"({"hits": {"hits": "bogus"}})", kActive, 2)
              .empty());
    CHECK(live::selectTopWinners(R"({"hits": {"hits": null}})", kActive, 2)
              .empty());
}

TEST_CASE("selectTopWinners skips behavioural clones with identical results "
          "tuples",
          "[liveWinners]") {
    // Re-running `load` mints a fresh UUID for the identical config; the
    // deterministic backtest then writes an identical results tuple. The
    // clone must not consume a slot even though its OHLC config is diverse.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-a", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 12.5, .tradesClosed = 40}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-clone", 50.0, true,
                {.ohlc = {series(20, 15)}, .finalPnl = 12.5, .tradesClosed = 40}),
        makeHit("r3", "EURUSD", "OhlcBreakoutStrategy", "u-b", 49.0, true,
                {.ohlc = {series(20, 30)}, .finalPnl = 8.0, .tradesClosed = 31}),
        makeHit("r4", "EURUSD", "OhlcBreakoutStrategy", "u-c", 48.0, true,
                {.ohlc = {series(20, 60)}, .finalPnl = 5.0, .tradesClosed = 22}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 3);
    CHECK(winners[0].config.UUID == "u-a");
    CHECK(winners[1].config.UUID == "u-b");
    CHECK(winners[2].config.UUID == "u-c");
}

TEST_CASE("selectTopWinners books a doubly-fetched multi-symbol document "
          "once per symbol",
          "[liveWinners]") {
    // fetchWinners queries per (strategy, symbol), so a multi-symbol
    // document matches each of its symbols' queries and lands in the merged
    // candidate map once per fetch. The copies are exact duplicates —
    // identical results tuples — and must dedup as behavioural clones, not
    // book two workers per group.
    const nlohmann::json hit =
        makeHit("r1", "EURUSD,USDJPY", "RandomStrategy", "u-multi", 30.0, true,
                {.finalPnl = 3.0, .tradesClosed = 7});
    const std::string body = makeResponse({hit, hit});

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].symbol == "EURUSD");
    CHECK(winners[1].symbol == "USDJPY");
    CHECK(winners[0].config.UUID == "u-multi");
    CHECK(winners[1].config.UUID == "u-multi");
}

TEST_CASE("selectTopWinners treats equal scores with differing finalPnl or "
          "tradesClosed as distinct, not clones",
          "[liveWinners]") {
    // Pins the dedup KEY: equal scores alone must not read as clones when
    // finalPnl or tradesClosed differ. The differences here clear the
    // behavioural neighbour bands so the diversity gate books both —
    // near-identical tuples are that gate's business, pinned by the
    // noise-floor tests below.
    const std::string pnlDiffers = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-1", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 1000.0, .tradesClosed = 40}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-2", 50.0, true,
                {.ohlc = {series(20, 15)}, .finalPnl = 2000.0, .tradesClosed = 40}),
    });
    CHECK(live::selectTopWinners(pnlDiffers, kActive, 3).size() == 2);

    const std::string tradesDiffer = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-1", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 12.5, .tradesClosed = 40}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-2", 50.0, true,
                {.ohlc = {series(20, 15)}, .finalPnl = 12.5, .tradesClosed = 60}),
    });
    CHECK(live::selectTopWinners(tradesDiffer, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners excludes behavioural neighbours whose results "
          "sit close on every axis",
          "[liveWinners]") {
    // LSR-shaped (2026-07-12 batch): a sweep-pips jiggle closed 41 vs 38
    // trades for 1692 vs 1610 PnL — the market barely noticed the knob.
    // The OHLC minutes DIFFER (5 vs 15), which the config-space gate called
    // diverse; behavioural closeness must win over config distance.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-top", 50.0, true,
                {.ohlc = {series(20, 5)},
                 .finalPnl = 1692.0, .tradesClosed = 41}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-jiggle", 49.0, true,
                {.ohlc = {series(20, 15)},
                 .finalPnl = 1610.0, .tradesClosed = 38}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 1);
    CHECK(winners[0].config.UUID == "u-top");
}

TEST_CASE("selectTopWinners admits candidates whose trade counts sit apart",
          "[liveWinners]") {
    // The VALID_BARS case (2026-07-12): 41 vs 60 trades at similar PnL is
    // a different behaviour. The configs share one bar series with counts
    // inside the legacy gap (20 vs 22 on 5m) — the old gate called this a
    // jiggle and booked one; the trades axis books both.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-41", 50.0, true,
                {.ohlc = {series(20, 5)},
                 .finalPnl = 1692.0, .tradesClosed = 41}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-60", 49.0, true,
                {.ohlc = {series(22, 5)},
                 .finalPnl = 1897.0, .tradesClosed = 60}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners admits candidates whose PnL sits apart beyond "
          "band and floor",
          "[liveWinners]") {
    // Same trade count; 1000 vs 2000 clears the relative band (20% of
    // 2000 = 400) and the absolute floor.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-1k", 50.0, true,
                {.finalPnl = 1000.0, .tradesClosed = 50}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-2k", 49.0, true,
                {.finalPnl = 2000.0, .tradesClosed = 50}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners treats sub-floor PnL differences as noise, not "
          "diversity",
          "[liveWinners]") {
    SECTION("10 vs 20: 100% apart relatively, spread noise absolutely") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "RandomStrategy", "u-20", 50.0, true,
                    {.finalPnl = 20.0, .tradesClosed = 50}),
            makeHit("r2", "EURUSD", "RandomStrategy", "u-10", 49.0, true,
                    {.finalPnl = 10.0, .tradesClosed = 50}),
        });
        const auto winners = live::selectTopWinners(body, kActive, 3);
        REQUIRE(winners.size() == 1);
        CHECK(winners[0].config.UUID == "u-20");
    }
    SECTION("a sign flip inside the noise floor is still noise") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "RandomStrategy", "u-plus", 50.0, true,
                    {.finalPnl = 5.0, .tradesClosed = 50}),
            makeHit("r2", "EURUSD", "RandomStrategy", "u-minus", 49.0, true,
                    {.finalPnl = -5.0, .tradesClosed = 50}),
        });
        CHECK(live::selectTopWinners(body, kActive, 3).size() == 1);
    }
}

TEST_CASE("selectTopWinners treats results exactly at the band as "
          "neighbours",
          "[liveWinners]") {
    // 100 vs 80 trades: the delta equals the 20% band exactly — distinct
    // requires STRICTLY exceeding it, the same boundary doctrine the
    // legacy count gap uses.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-100", 50.0, true,
                {.finalPnl = 1600.0, .tradesClosed = 100}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-80", 49.0, true,
                {.finalPnl = 1700.0, .tradesClosed = 80}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 1);
}

TEST_CASE("selectTopWinners applies the absolute trade floor to small "
          "counts",
          "[liveWinners]") {
    // 5 vs 8 trades is 60% apart relatively, but small counts are noisy —
    // the absolute floor says neighbour.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-5", 50.0, true,
                {.finalPnl = 600.0, .tradesClosed = 5}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-8", 49.0, true,
                {.finalPnl = 700.0, .tradesClosed = 8}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 1);
}

TEST_CASE("selectTopWinners admits a flipped long/short mix as diversity",
          "[liveWinners]") {
    // 40L/10S vs 10L/40S on near-identical counts and PnL: two
    // parameterisations trading opposite sides of the same market are
    // partially hedging — exactly the pair diversification wants kept.
    // Without the direction axis these would merge as neighbours.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-long", 50.0, true,
                {.finalPnl = 1500.0, .tradesClosed = 50,
                 .openedLong = 40, .openedShort = 10}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-short", 49.0, true,
                {.finalPnl = 1520.0, .tradesClosed = 50,
                 .openedLong = 10, .openedShort = 40}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners stands the direction axis down when a document "
          "predates the opened counts",
          "[liveWinners]") {
    // Mid-vintage doc: carries tradesClosed but not openedLong/Short. The
    // axis cannot claim distinctness it cannot measure, so the pair is
    // judged on the remaining axes — close there, one booked.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-new", 50.0, true,
                {.finalPnl = 1500.0, .tradesClosed = 50,
                 .openedLong = 40, .openedShort = 10}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-mid", 49.0, true,
                {.finalPnl = 1520.0, .tradesClosed = 50}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 1);
    CHECK(winners[0].config.UUID == "u-new");
}

TEST_CASE("selectTopWinners requires behavioural diversity against every "
          "already-picked winner",
          "[liveWinners]") {
    // A(100 trades) books; B(140) clears A (delta 40 > 20% of 140 = 28);
    // C(120) sits within the band of BOTH picks (20 <= 24 vs A, 20 <= 28
    // vs B) — skipped; D(180) clears both (80 and 40 > 36) and takes the
    // third slot. PnL identical throughout so only the trades axis moves.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-100", 50.0, true,
                {.finalPnl = 2000.0, .tradesClosed = 100}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-140", 49.0, true,
                {.finalPnl = 2000.0, .tradesClosed = 140}),
        makeHit("r3", "EURUSD", "RandomStrategy", "u-120", 48.0, true,
                {.finalPnl = 2000.0, .tradesClosed = 120}),
        makeHit("r4", "EURUSD", "RandomStrategy", "u-180", 47.0, true,
                {.finalPnl = 2000.0, .tradesClosed = 180}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 3);
    CHECK(winners[0].config.UUID == "u-100");
    CHECK(winners[1].config.UUID == "u-140");
    CHECK(winners[2].config.UUID == "u-180");
}

TEST_CASE("selectTopWinners falls back to the config gate when either "
          "document predates tradesClosed",
          "[liveWinners]") {
    // Mixed pair: one old-shape doc (no tradesClosed -> -1 fallback), one
    // behavioural doc. The pair cannot be judged behaviourally, so the
    // legacy config-space gate governs it in both directions.
    SECTION("differing bar minutes: config gate books both") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-old", 30.0,
                    true, {.ohlc = {series(20, 5)}}),
            makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-new", 29.0,
                    true,
                    {.ohlc = {series(20, 15)},
                     .finalPnl = 1000.0, .tradesClosed = 40}),
        });
        CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
    }
    SECTION("same series within the count gap: config gate books one") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-old", 30.0,
                    true, {.ohlc = {series(20, 5)}}),
            makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-new", 29.0,
                    true,
                    {.ohlc = {series(25, 5)},
                     .finalPnl = 1000.0, .tradesClosed = 40}),
        });
        const auto winners = live::selectTopWinners(body, kActive, 3);
        REQUIRE(winners.size() == 1);
        CHECK(winners[0].config.UUID == "u-old");
    }
}

TEST_CASE("selectTopWinners dedups documents missing finalPnl/tradesClosed "
          "on the fallback key",
          "[liveWinners]") {
    // Two old-shape documents (results carries only performanceScore) with
    // the same score collide on the fallback key and dedup to one...
    const std::string oldClones = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-1", 30.0, true,
                {.ohlc = {series(20, 5)}}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-2", 30.0, true,
                {.ohlc = {series(20, 15)}}),
    });
    const auto deduped = live::selectTopWinners(oldClones, kActive, 3);
    REQUIRE(deduped.size() == 1);
    CHECK(deduped[0].config.UUID == "u-1");

    // ...but a real finalPnl differs from the 0.0 fallback, so an old doc
    // and a new same-score doc stay distinct.
    const std::string oldVsNew = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-old", 30.0, true,
                {.ohlc = {series(20, 5)}}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-new", 30.0, true,
                {.ohlc = {series(20, 15)}, .finalPnl = 1.0}),
    });
    CHECK(live::selectTopWinners(oldVsNew, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners rejects same-minutes candidates within the count "
          "gap as not diverse",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-20", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-25", 49.0, true,
                {.ohlc = {series(25, 5)}, .finalPnl = 2.0}),
        makeHit("r3", "EURUSD", "OhlcBreakoutStrategy", "u-35", 48.0, true,
                {.ohlc = {series(35, 5)}, .finalPnl = 3.0}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    // 25 sits 5 bars from the picked 20 (a parameter jiggle); 35 sits 15
    // away and earns the second slot.
    REQUIRE(winners.size() == 2);
    CHECK(winners[0].config.UUID == "u-20");
    CHECK(winners[1].config.UUID == "u-35");
}

TEST_CASE("selectTopWinners treats differing bar minutes as diverse "
          "regardless of count",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-m5", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-m15", 49.0, true,
                {.ohlc = {series(20, 15)}, .finalPnl = 2.0}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners counts one differing series element as diverse "
          "in multi-series configs",
          "[liveWinners]") {
    // FVG-shaped: [0] scan series, [1] HTF series. Element 0 sits within the
    // count gap, but element 1's minutes differ — diverse.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-htf60", 50.0, true,
                {.ohlc = {series(20, 5), series(50, 60)}, .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-htf240", 49.0, true,
                {.ohlc = {series(22, 5), series(50, 240)}, .finalPnl = 2.0}),
    });

    CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners treats differing series counts as diverse, "
          "including empty vs non-empty",
          "[liveWinners]") {
    const std::string oneVsTwo = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-one", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-two", 49.0, true,
                {.ohlc = {series(20, 5), series(50, 60)}, .finalPnl = 2.0}),
    });
    CHECK(live::selectTopWinners(oneVsTwo, kActive, 3).size() == 2);

    // The both-empty guard must not swallow empty-vs-non-empty.
    const std::string emptyVsOne = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-none", 50.0, true,
                {.finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-bars", 49.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 2.0}),
    });
    CHECK(live::selectTopWinners(emptyVsOne, kActive, 3).size() == 2);
}

TEST_CASE("selectTopWinners lets empty-OHLC strategies through on clone "
          "dedup alone",
          "[liveWinners]") {
    // RandomStrategy builds no bars: both-empty OHLC is vacuously diverse,
    // so only the results tuple separates candidates.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-1", 50.0, true,
                {.finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-2", 49.0, true,
                {.finalPnl = 2.0}),
        makeHit("r3", "EURUSD", "RandomStrategy", "u-3", 50.0, true,
                {.finalPnl = 1.0}),  // clone of u-1
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].config.UUID == "u-1");
    CHECK(winners[1].config.UUID == "u-2");
}

TEST_CASE("selectTopWinners requires diversity against every already-picked "
          "winner",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-20", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-45", 49.0, true,
                {.ohlc = {series(45, 5)}, .finalPnl = 2.0}),
        makeHit("r3", "EURUSD", "OhlcBreakoutStrategy", "u-40", 48.0, true,
                {.ohlc = {series(40, 5)}, .finalPnl = 3.0}),
        makeHit("r4", "EURUSD", "OhlcBreakoutStrategy", "u-60", 47.0, true,
                {.ohlc = {series(60, 5)}, .finalPnl = 4.0}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    // 40 clears the gap against 20 but sits 5 from the picked 45 — skipped;
    // 60 clears both picked candidates and takes the third slot.
    REQUIRE(winners.size() == 3);
    CHECK(winners[0].config.UUID == "u-20");
    CHECK(winners[1].config.UUID == "u-45");
    CHECK(winners[2].config.UUID == "u-60");
}

TEST_CASE("selectTopWinners books fewer than topPerGroup when the group "
          "lacks diverse survivors",
          "[liveWinners]") {
    // Every candidate sits within the count gap of the top pick — including
    // the exact-boundary |30 - 20| == 10, which is NOT diverse (the gap is
    // strictly-greater-than). No backfill: one winner, not three.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-20", 50.0, true,
                {.ohlc = {series(20, 5)}, .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-22", 49.0, true,
                {.ohlc = {series(22, 5)}, .finalPnl = 2.0}),
        makeHit("r3", "EURUSD", "OhlcBreakoutStrategy", "u-25", 48.0, true,
                {.ohlc = {series(25, 5)}, .finalPnl = 3.0}),
        makeHit("r4", "EURUSD", "OhlcBreakoutStrategy", "u-28", 47.0, true,
                {.ohlc = {series(28, 5)}, .finalPnl = 4.0}),
        makeHit("r5", "EURUSD", "OhlcBreakoutStrategy", "u-30", 46.0, true,
                {.ohlc = {series(30, 5)}, .finalPnl = 5.0}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 1);
    CHECK(winners[0].config.UUID == "u-20");
}

TEST_CASE("selectTopWinners carries finalPnl and tradesClosed onto the "
          "Winner, defaulting old documents",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-new", 30.0, true,
                {.finalPnl = 7.25, .tradesClosed = 19}),
        makeHit("r2", "USDJPY", "RandomStrategy", "u-old", 30.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].config.UUID == "u-new");
    CHECK(winners[0].finalPnl == 7.25);
    CHECK(winners[0].tradesClosed == 19);
    // The fallbacks: 0.0 PnL, -1 trades — -1 so an old document can never
    // collide with a genuine 0-trade run.
    CHECK(winners[1].config.UUID == "u-old");
    CHECK(winners[1].finalPnl == 0.0);
    CHECK(winners[1].tradesClosed == -1);
}

TEST_CASE("selectTopWinners treats range-bar winners on one series identity "
          "as parameter jiggles, not diverse",
          "[liveWinners]") {
    // Same (TICK_WINDOW, PERCENT); only the derived RANGE_COUNT differs —
    // comparing it would mistake a derivation artefact for diversity. OHLC
    // is empty on both (the range-bar strategy shape), and the results
    // tuples differ so clone dedup cannot mask the diversity verdict.
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-1", 50.0, true,
                {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                            .RANGE_ATR_PERCENT = 40,
                            .RANGE_COUNT = 37}},
                 .finalPnl = 1.0}),
        makeHit("r2", "EURUSD", "RandomStrategy", "u-2", 49.0, true,
                {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                            .RANGE_ATR_PERCENT = 40,
                            .RANGE_COUNT = 9}},
                 .finalPnl = 2.0}),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 1);
    CHECK(winners[0].config.UUID == "u-1");
}

TEST_CASE("selectTopWinners treats differing range window or percent as "
          "diverse",
          "[liveWinners]") {
    SECTION("TICK_WINDOW differs") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "RandomStrategy", "u-1", 50.0, true,
                    {.range = {{.RANGE_ATR_TICK_WINDOW = 2500,
                                .RANGE_ATR_PERCENT = 40,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 1.0}),
            makeHit("r2", "EURUSD", "RandomStrategy", "u-2", 49.0, true,
                    {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                                .RANGE_ATR_PERCENT = 40,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 2.0}),
        });
        CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
    }
    SECTION("PERCENT differs") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "RandomStrategy", "u-1", 50.0, true,
                    {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                                .RANGE_ATR_PERCENT = 25,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 1.0}),
            makeHit("r2", "EURUSD", "RandomStrategy", "u-2", 49.0, true,
                    {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                                .RANGE_ATR_PERCENT = 40,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 2.0}),
        });
        CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
    }
    SECTION("range series present vs absent") {
        // Both OHLC-empty: without the range axis these would be vacuously
        // diverse-by-emptiness; the size mismatch is real diversity.
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "RandomStrategy", "u-range", 50.0, true,
                    {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                                .RANGE_ATR_PERCENT = 40,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 1.0}),
            makeHit("r2", "EURUSD", "RandomStrategy", "u-none", 49.0, true,
                    {.finalPnl = 2.0}),
        });
        CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
    }
    SECTION("OHLC diversity still wins when the range series are identical") {
        const std::string body = makeResponse({
            makeHit("r1", "EURUSD", "OhlcBreakoutStrategy", "u-m5", 50.0, true,
                    {.ohlc = {series(20, 5)},
                     .range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                                .RANGE_ATR_PERCENT = 40,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 1.0}),
            makeHit("r2", "EURUSD", "OhlcBreakoutStrategy", "u-m15", 49.0, true,
                    {.ohlc = {series(20, 15)},
                     .range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                                .RANGE_ATR_PERCENT = 40,
                                .RANGE_COUNT = 37}},
                     .finalPnl = 2.0}),
        });
        CHECK(live::selectTopWinners(body, kActive, 3).size() == 2);
    }
}

TEST_CASE("selectTopWinners carries RANGE_VARIABLES and defaults documents "
          "that predate them",
          "[liveWinners]") {
    const std::string body = makeResponse({
        makeHit("r1", "EURUSD", "RandomStrategy", "u-range", 30.0, true,
                {.range = {{.RANGE_ATR_TICK_WINDOW = 5000,
                            .RANGE_ATR_PERCENT = 40,
                            .RANGE_COUNT = 50}},
                 .finalPnl = 1.0}),
        makeHit("r2", "USDJPY", "RandomStrategy", "u-old", 30.0),
    });

    const auto winners = live::selectTopWinners(body, kActive, 3);

    REQUIRE(winners.size() == 2);
    CHECK(winners[0].config.UUID == "u-range");
    REQUIRE(winners[0].config.RANGE_VARIABLES.size() == 1);
    CHECK(winners[0].config.RANGE_VARIABLES[0].RANGE_ATR_TICK_WINDOW == 5000);
    CHECK(winners[0].config.RANGE_VARIABLES[0].RANGE_ATR_PERCENT == 40);
    CHECK(winners[0].config.RANGE_VARIABLES[0].RANGE_COUNT == 50);
    // A pre-range document parses with the field empty, not a parse failure
    // that would silently drop the winner.
    CHECK(winners[1].config.UUID == "u-old");
    CHECK(winners[1].config.RANGE_VARIABLES.empty());
}

TEST_CASE("buildWinnersQueryBody terms the strategy name on the .keyword "
          "subfield and matches the symbol on the analyzed field",
          "[liveWinners]") {
    // The strategy base field is analyzed text: a term against it silently
    // matches nothing and live starts with zero winners. The symbol clause
    // has the opposite shape — a match on the ANALYZED config.SYMBOLS, NOT a
    // .keyword term, which would silently drop legacy multi-symbol documents
    // ("EURUSD,USDJPY"). This pins both paths and the query's load-bearing
    // clauses (not the page size — config).
    const auto body = nlohmann::json::parse(
        live::buildWinnersQueryBody(12.5, 5.0, 30.0, "FvgStrategy",
                                    "EURUSD"));

    const auto& boolQuery = body.at("query").at("bool");
    const auto& filter = boolQuery.at("filter");
    REQUIRE(filter.is_array());

    bool sawStrategyTerm = false;
    bool sawScoreFloor = false;
    bool sawDrawdownCeiling = false;
    bool sawCalmarFloor = false;
    bool sawSymbolMatch = false;
    for (const auto& clause : filter) {
        if (clause.contains("term")
            && clause["term"].contains(
                "config.STRATEGY.TRADING_VARIABLES.STRATEGY.keyword")) {
            sawStrategyTerm = true;
            CHECK(clause["term"]
                      ["config.STRATEGY.TRADING_VARIABLES.STRATEGY.keyword"]
                  == "FvgStrategy");
        }
        if (clause.contains("range")
            && clause["range"].contains("results.performanceScore")) {
            sawScoreFloor = true;
            CHECK(clause["range"]["results.performanceScore"]["gt"] == 12.5);
        }
        // The ceiling must be lte INSIDE the filter array: a should/penalty
        // shape would let a spiky run through on score alone, and a missing
        // maxDrawdownPercent field must fail the filter (fail-closed).
        if (clause.contains("range")
            && clause["range"].contains("results.maxDrawdownPercent")) {
            sawDrawdownCeiling = true;
            CHECK(clause["range"]["results.maxDrawdownPercent"]["lte"]
                  == 5.0);
        }
        // Same fail-closed shape for the calmar floor: gte inside filter.
        if (clause.contains("range")
            && clause["range"].contains("results.calmarScore")) {
            sawCalmarFloor = true;
            CHECK(clause["range"]["results.calmarScore"]["gte"] == 30.0);
        }
        if (clause.contains("match")
            && clause["match"].contains("config.SYMBOLS")) {
            sawSymbolMatch = true;
            CHECK(clause["match"]["config.SYMBOLS"] == "EURUSD");
        }
    }
    CHECK(sawStrategyTerm);
    CHECK(sawScoreFloor);
    CHECK(sawDrawdownCeiling);
    CHECK(sawCalmarFloor);
    CHECK(sawSymbolMatch);

    const auto& mustNot = boolQuery.at("must_not");
    REQUIRE(mustNot.is_array());
    REQUIRE(mustNot.size() == 1);
    CHECK(mustNot[0].contains("range"));
    CHECK(mustNot[0]["range"].contains("config.OFFSET_MONTHS"));

    const auto& sort = body.at("sort");
    REQUIRE(sort.is_array());
    CHECK(sort[0] == nlohmann::json{{"results.performanceScore", "desc"}});
}
