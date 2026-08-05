// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveWinners — pulls the winning backtest runs out of Elasticsearch so the
// live subcommand can trade them. The read goes through the rolling
// backtesting-winners-current alias (outcome_index::currentAlias), which the
// weekly load repoints at that week's winners index — so live selects from
// the MOST RECENT weekly batch only, and a past winner keeps trading only by
// re-proving itself each week. A "winner" is a run whose
// results.performanceScore cleared the minimum, whose
// results.maxDrawdownPercent stayed at or under the ceiling (a HARD gate on
// peak-to-trough giveback — the Calmar half of the score only blends the
// drawdown in, so a high-expectancy spiky run can out-score it), whose
// results.calmarScore cleared its floor (growth per unit of drawdown — the
// two drawdown gates catch different shapes: the floor rejects smooth but
// stagnant runs, the ceiling rejects fast growers with deep absolute
// givebacks), AND that ran over the full-history window
// (rolling::kFullHistory — LAST_MONTHS = 9, OFFSET_MONTHS = 0): the
// ladder's shorter 3-month rungs are screening passes, not evidence a
// config should trade live. The window filters are
// kept even though the weekly winners index only holds full-history runs —
// during rollout the alias may be hand-parked on the mixed-rung legacy
// trading_results index (see QUICKSTART), and the filters cost nothing after.
// live keeps the top N runs per (symbol, strategy) pair, restricted to the
// caller's active strategy names.
//
// The fetch is one _search PER (active strategy, tradable symbol) pair, not
// one page per strategy: a score-sorted page shared across symbols is
// monopolised by whichever symbol sweeps hottest (observed 2026-07-12:
// FvgStrategy's 12.3k qualifying docs put its top-1000 cutoff at score ~51,
// starving 10 of its 15 winning symbols before grouping ever saw them — the
// same starvation the old one-global-page fetch inflicted across
// strategies). A multi-symbol document matches each of its symbols' queries
// and lands in the merged group map once per fetch; those copies are exact
// duplicates, so the behavioural-clone dedup below removes them.
// Selection within a group also enforces behavioural diversity —
// re-running `load` mints fresh UUIDs for byte-identical configs, and
// near-identical configs backtest near-identically, so a plain top-N books
// the same behaviour several times over (see sameResults / isDiverse below).
//
// Parsing deliberately touches only config.SYMBOLS, config.STRATEGY, RUN_ID,
// results.performanceScore/finalPnl/tradesClosed and the two integer risk
// caps (MAX_OPEN_TRADES, MAX_TRADES_PER_MINUTE) — never the whole
// Configuration. The reporting path
// re-types STARTING_BALANCE / MAX_LOSS_PERCENT as JSON numbers for Kibana
// (see reportConfigJson in tradingResults.cpp), which the shared
// string-encoded decimal from_json would reject. config.STRATEGY is safe both
// ways (readIntField accepts numbers or strings), and the caps are plain
// JSON ints in both encodings.

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"
#include "run/reporting/outcomeIndices.hpp"
#include "shared/tradingDefinitions/config/runConfiguration.hpp"
#include "shared/tradingDefinitions/strategyConfig.hpp"
#include "shared/utilities/backtestLog.hpp"

export module liveWinners;

import std;            // replaces <algorithm>, <map>, <span>, <string>, <vector>
import rollingWindow;  // rolling::kFullHistory — the ladder's terminal full run
import symbolGroups;   // sweep::splitSymbols — the same trim+split the sweeps use

export namespace live {

// One tradable winner: a past run's full StrategyConfig bound to a single
// symbol (multi-symbol runs are split into one Winner per symbol).
struct Winner {
    std::string symbol;
    std::string strategyName;  // == config.TRADING_VARIABLES.STRATEGY
    std::string runId;         // provenance for logs
    double performanceScore{0.0};
    // Behavioural provenance, doing double duty at selection time:
    // the exact tuple (performanceScore, finalPnl, tradesClosed) is the
    // clone key (re-running `load` mints fresh UUIDs for byte-identical
    // configs, and near-identical configs can backtest byte-identically),
    // and finalPnl/tradesClosed plus the long/short mix below are the
    // diversity axes (see isDiverse). The strategy runner never reads any
    // of them. -1 marks a document that predates the field, so an old doc
    // and a genuine 0-trade run never collide on the fallback key, and
    // isDiverse knows when a field is unavailable rather than zero.
    double finalPnl{0.0};
    long long tradesClosed{-1};
    long long openedLong{-1};
    long long openedShort{-1};
    // Run-level risk caps the winning run backtested under, enforced live by
    // the strategy runner. Defaults mirror tradingDefinitions::
    // RunConfiguration for documents written before the caps existed.
    int maxOpenTrades{0};
    int maxTradesPerMinute{60};
    bool peakHoursOnly{false};
    tradingDefinitions::StrategyConfig config;
};

// Pure selection over an Elasticsearch _search response body (no I/O, unit
// tested with synthetic fixtures): keep hits whose strategy name is in
// `activeStrategies`, split comma-separated config.SYMBOLS into one candidate
// per symbol, and pick up to `topPerGroup` per (symbol, strategy) by
// performanceScore descending, subject to two constraints against every
// already-picked candidate in the group: not a behavioural clone (identical
// results tuple) and behaviourally diverse (the results sit apart on at
// least one axis — trade count, PnL, or long/short mix; see isDiverse).
// A group with fewer diverse survivors books fewer — a backfilled
// neighbour adds risk concentration, not coverage. A malformed hit is
// logged and skipped — one bad historical document must not kill live
// startup. Output is ordered by symbol, then strategy name, then score
// descending, so startup logs and tests are deterministic.
std::vector<Winner> selectTopWinners(
    const std::string& searchResponseBody,
    std::span<const std::string_view> activeStrategies,
    std::size_t topPerGroup);

// The _search body fetchWinners issues for one (strategy, symbol) pair: the
// score floor, drawdown ceiling, calmar floor and full-history window
// filters, a term on the .keyword subfield of the strategy name — the base field is analyzed
// text, so a term there silently matches nothing and live would start with
// zero winners and no clue why — and a match on the ANALYZED config.SYMBOLS
// field, which is deliberately not the .keyword: a term there would silently
// drop legacy multi-symbol documents ("EURUSD,USDJPY"), where the analyzer's
// tokenisation still hits each symbol. Exported so a test can pin both
// paths.
std::string buildWinnersQueryBody(double minScore,
                                  double maxDrawdownPercent,
                                  double minCalmarScore,
                                  std::string_view strategyName,
                                  std::string_view symbol);

// Startup fetch: one _search on the backtesting-winners-current alias PER
// (active strategy, symbol) pair, each filtered server-side to
// performanceScore > minScore AND maxDrawdownPercent <= maxDrawdownPercent
// AND calmarScore >= minCalmarScore AND the full-history window
// (rolling::kFullHistory), sorted by score descending, so if more than the
// request size qualify, the truncation keeps that pair's best. `symbols`
// is the live allowlist (live::kTradableSymbols) — a winner on any other
// symbol could only book a worker whose orders the channel drops. A pair
// whose query or parse fails is logged and skipped — trading the remaining
// pairs beats trading none, and a persistent outage fails every query so
// the result degenerates to the empty vector the caller already treats as
// fatal.
std::vector<Winner> fetchWinners(double minScore, double maxDrawdownPercent,
                                 double minCalmarScore,
                                 std::size_t topPerGroup,
                                 std::span<const std::string_view> activeStrategies,
                                 std::span<const std::string_view> symbols);

}  // namespace live

namespace {

// Upper bound on hits fetched per (strategy, symbol) startup query. The page
// is sorted by score, so truncation keeps that pair's best 5000 — sized above
// the largest group observed so far (4,345 docs, FvgStrategy/DEUIDXEUR,
// 2026-07-12) and under Elasticsearch's default 10k max_result_window; if a
// sweep outgrows it, fetchWinners logs the truncation loudly per pair.
constexpr std::size_t kMaxHits = 5000;

// LEGACY FALLBACK ONLY (see isDiverse): config-space diversity governs a
// pair only when either document predates results.tradesClosed. For those
// documents, a candidate whose OHLC_COUNT sits within this many bars of a
// picked one (on the same OHLC_MINUTES) is a parameter jiggle, not a
// distinct strategy: the count is derived from the real knob
// (LOOKBACK_BARS + 3, SMA period + 2, ...) for every strategy except
// ohlcBreakout — so the gap indirectly diversifies those drivers too.
constexpr int kOhlcCountDiversityGap = 10;

// Behavioural neighbourhood: two candidates are the same behaviour unless
// their results sit apart on at least one axis (trade count, PnL,
// long/short mix). The band is relative — kBehaviourNeighbourFrac of the
// larger value — with absolute floors so relative maths cannot manufacture
// diversity out of noise: small trade counts are noisy in relative terms,
// and sub-floor PnL differences are spread/slippage noise, not behaviour
// (a 10-vs-20 PnL pair is one strategy twice, and so is +5 vs -5 — the
// floor subsumes the sign question, while +1000 vs -1000 clears both
// bounds and stays distinct). Distinct requires STRICTLY exceeding the
// bound, mirroring the legacy count-gap boundary doctrine. kMinPnlGap
// assumes account-currency PnL at the observed winner scale (9-month PnLs
// ~1-2k, 2026-07-12 batch); revisit if TRADING_SIZE or the account scale
// shifts.
constexpr double kBehaviourNeighbourFrac = 0.20;
constexpr long long kMinTradeGap = 3;
constexpr double kMinPnlGap = 500.0;

// Shape-checked walk to hits.hits. Elasticsearch always returns that
// envelope, but a 2xx body from something else (a proxy's error JSON, a
// non-object top level, {"hits":null}) must take the empty-return path — a
// throwing accessor here would escape selectTopWinners' per-hit guard and
// take live startup down with an uncaught type_error. Returns a pointer into
// `response` (no copy of the hits tree).
const nlohmann::json* findHitsArray(const nlohmann::json& response) {
    if (!response.is_object()) {
        return nullptr;
    }
    const auto hits = response.find("hits");
    if (hits == response.end() || !hits->is_object()) {
        return nullptr;
    }
    const auto list = hits->find("hits");
    if (list == hits->end() || !list->is_array()) {
        return nullptr;
    }
    return &*list;
}

}  // namespace

namespace live {

// (symbol, strategyName) -> candidates, shared by the collect and pick
// phases so fetchWinners can merge its per-strategy responses into one map
// before picking — std::map iteration keeps the output ordered by symbol
// then strategy name, with no concatenate-then-resort pass.
using GroupMap =
    std::map<std::pair<std::string, std::string>, std::vector<Winner>>;

// Behavioural-clone test: an identical results tuple means the market never
// noticed the difference between the two configs. Exact double equality is
// deliberate — clones are byte-identical documents, not merely close ones.
static bool sameResults(const Winner& a, const Winner& b) {
    return std::tie(a.performanceScore, a.finalPnl, a.tradesClosed)
           == std::tie(b.performanceScore, b.finalPnl, b.tradesClosed);
}

// True when the two configs' OHLC series differ meaningfully: any element
// differs in OHLC_MINUTES (the axis that actually moves results), or (same
// minutes) the counts sit more than kOhlcCountDiversityGap apart. Different
// series counts are trivially diverse. Equal-and-empty is NOT diversity —
// legacyConfigDiverse below owns that judgement across both bar types.
static bool ohlcDiffers(const std::vector<tradingDefinitions::OHLCVariables>& x,
                        const std::vector<tradingDefinitions::OHLCVariables>& y) {
    if (x.size() != y.size()) {
        return true;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i].OHLC_MINUTES != y[i].OHLC_MINUTES) {
            return true;
        }
        if (std::abs(x[i].OHLC_COUNT - y[i].OHLC_COUNT)
            > kOhlcCountDiversityGap) {
            return true;
        }
    }
    return false;
}

// Range-bar analogue: TICK_WINDOW and PERCENT are the real knobs (the series
// identity); RANGE_COUNT is ignored because the makers derive it from the
// strategy's scan depths — comparing it would mistake a derivation artefact
// for diversity.
static bool rangeDiffers(
    const std::vector<tradingDefinitions::RangeBarVariables>& x,
    const std::vector<tradingDefinitions::RangeBarVariables>& y) {
    if (x.size() != y.size()) {
        return true;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i].RANGE_ATR_TICK_WINDOW != y[i].RANGE_ATR_TICK_WINDOW ||
            x[i].RANGE_ATR_PERCENT != y[i].RANGE_ATR_PERCENT) {
            return true;
        }
    }
    return false;
}

// Legacy config-space diversity, kept ONLY for documents that predate
// results.tradesClosed (isDiverse dispatches here when either side carries
// the -1 fallback): diverse when either bar-series axis differs
// meaningfully. Identical shapes on both axes are vacuously diverse ONLY
// when neither config builds any bars at all (RandomStrategy) — clone
// dedup is the sole guard there.
static bool legacyConfigDiverse(const Winner& a, const Winner& b) {
    if (ohlcDiffers(a.config.OHLC_VARIABLES, b.config.OHLC_VARIABLES)) {
        return true;
    }
    if (rangeDiffers(a.config.RANGE_VARIABLES, b.config.RANGE_VARIABLES)) {
        return true;
    }
    return a.config.OHLC_VARIABLES.empty() && b.config.OHLC_VARIABLES.empty() &&
           a.config.RANGE_VARIABLES.empty() && b.config.RANGE_VARIABLES.empty();
}

// Trade-count axis: close when the difference sits inside the relative
// band or the absolute floor (see the kBehaviour* comment).
static bool tradesClose(const Winner& a, const Winner& b) {
    const long long delta = a.tradesClosed >= b.tradesClosed
                                ? a.tradesClosed - b.tradesClosed
                                : b.tradesClosed - a.tradesClosed;
    const double band = std::max(
        static_cast<double>(kMinTradeGap),
        kBehaviourNeighbourFrac
            * static_cast<double>(std::max(a.tradesClosed, b.tradesClosed)));
    return static_cast<double>(delta) <= band;
}

// PnL axis: same shape. The floor makes sign flips inside the noise band
// close (+5 vs -5) while materially opposite results stay distinct
// (+1000 vs -1000 has delta 2000, clearing both bounds).
static bool pnlClose(const Winner& a, const Winner& b) {
    const double band = std::max(
        kMinPnlGap,
        kBehaviourNeighbourFrac
            * std::max(std::abs(a.finalPnl), std::abs(b.finalPnl)));
    return std::abs(a.finalPnl - b.finalPnl) <= band;
}

// Long/short-mix axis: close when the long fractions sit within the band.
// Two parameterisations of one strategy can flip directional emphasis while
// landing on near-identical counts and PnL — partially hedging pairs the
// portfolio wants to keep, so a flipped mix is diversity. The axis cannot
// claim distinctness when either side lacks the fields (mid-vintage
// documents, -1) or opened nothing — it reads close and leaves the verdict
// to the other axes.
static bool directionClose(const Winner& a, const Winner& b) {
    if (a.openedLong < 0 || a.openedShort < 0 || b.openedLong < 0 ||
        b.openedShort < 0) {
        return true;
    }
    const long long aTotal = a.openedLong + a.openedShort;
    const long long bTotal = b.openedLong + b.openedShort;
    if (aTotal == 0 || bTotal == 0) {
        return true;
    }
    const double aFrac =
        static_cast<double>(a.openedLong) / static_cast<double>(aTotal);
    const double bFrac =
        static_cast<double>(b.openedLong) / static_cast<double>(bTotal);
    return std::abs(aFrac - bFrac) <= kBehaviourNeighbourFrac;
}

// Diverse when the two candidates' RESULTS sit apart on any axis — the
// backtest already measured whether the market distinguished them, which is
// what config-space identity used to approximate (and misjudged both ways,
// observed 2026-07-12: LSR configs whose VALID_BARS flip closed 41 vs 60
// trades on one 60m series — distinct behaviour read as a jiggle — while
// two Fvg configs sharing a 15m scan series and differing only in HTF
// filter traded near-identical books yet read as diverse). Neighbour =
// close on ALL axes; the config check survives solely for documents too
// old to carry the behavioural fields.
static bool isDiverse(const Winner& a, const Winner& b) {
    if (a.tradesClosed < 0 || b.tradesClosed < 0) {
        return legacyConfigDiverse(a, b);
    }
    return !(tradesClose(a, b) && pnlClose(a, b) && directionClose(a, b));
}

// Parsing half of selection — module-private so fetchWinners (which also
// reads hits.total) parses each body exactly once, appending one Winner per
// (hit x symbol) into `groups` across calls. Not an exported overload of
// selectTopWinners: a string literal converts equally well to std::string
// and nlohmann::json, so an overload pair would be ambiguous at every
// literal call site.
static void collectCandidates(
    const nlohmann::json& searchResponse,
    std::span<const std::string_view> activeStrategies, GroupMap& groups) {
    const nlohmann::json* hitsArray = findHitsArray(searchResponse);
    if (hitsArray == nullptr) {
        backtest_log::error(
            "liveWinners: _search response is not an Elasticsearch hits "
            "envelope; treating as no winners");
        return;
    }

    // Fallbacks for documents predating the risk caps — the same defaults the
    // backtest itself would have run those configs under.
    const tradingDefinitions::RunConfiguration riskDefaults{};

    for (const auto& hit : *hitsArray) {
        try {
            const nlohmann::json& source = hit.at("_source");
            const nlohmann::json& config = source.at("config");
            const nlohmann::json& results = source.at("results");

            Winner base;
            base.runId = source.value("RUN_ID", "");
            base.performanceScore =
                results.at("performanceScore").get<double>();
            // Clone-key / diversity-axis fields: absent (or non-numeric) in
            // documents that predate them — fall back rather than let the
            // per-hit guard discard the whole hit.
            if (const auto it = results.find("finalPnl");
                it != results.end() && it->is_number()) {
                base.finalPnl = it->get<double>();
            }
            if (const auto it = results.find("tradesClosed");
                it != results.end() && it->is_number()) {
                base.tradesClosed = it->get<long long>();
            }
            if (const auto it = results.find("openedLong");
                it != results.end() && it->is_number()) {
                base.openedLong = it->get<long long>();
            }
            if (const auto it = results.find("openedShort");
                it != results.end() && it->is_number()) {
                base.openedShort = it->get<long long>();
            }
            base.config = config.at("STRATEGY")
                              .get<tradingDefinitions::StrategyConfig>();
            base.strategyName = base.config.TRADING_VARIABLES.STRATEGY;
            base.maxOpenTrades =
                config.value("MAX_OPEN_TRADES", riskDefaults.MAX_OPEN_TRADES);
            base.maxTradesPerMinute = config.value(
                "MAX_TRADES_PER_MINUTE", riskDefaults.MAX_TRADES_PER_MINUTE);
            base.peakHoursOnly =
                config.value("PEAK_HOURS_ONLY", riskDefaults.PEAK_HOURS_ONLY);

            if (std::ranges::find(activeStrategies, base.strategyName)
                == activeStrategies.end()) {
                continue;
            }

            // sweep::splitSymbols trims stray whitespace and drops empty
            // fields, exactly like the sweep side — an untrimmed " USDJPY"
            // would cache a worker no decoded tick can ever route to. The
            // returned views point into symbolsField, hence the named local.
            const auto symbolsField = config.at("SYMBOLS").get<std::string>();
            for (const std::string_view symbol :
                 sweep::splitSymbols(symbolsField)) {
                Winner w = base;
                w.symbol = std::string{symbol};
                groups[{w.symbol, w.strategyName}].push_back(std::move(w));
            }
        } catch (const std::exception& e) {
            backtest_log::error(std::string("liveWinners: skipping malformed hit (")
                                + e.what() + "): " + hit.dump());
        }
    }
}

// Ranking half: per group, walk candidates by score descending and keep up
// to topPerGroup that are neither behavioural clones of (sameResults) nor
// parameter jiggles on (isDiverse) every already-kept candidate.
static std::vector<Winner> pickWinners(GroupMap& groups,
                                       const std::size_t topPerGroup) {
    std::vector<Winner> winners;
    for (auto& [key, candidates] : groups) {
        // stable_sort keeps document order for equal scores, so ties resolve
        // the same way every startup.
        std::ranges::stable_sort(candidates, [](const Winner& a, const Winner& b) {
            return a.performanceScore > b.performanceScore;
        });
        std::vector<Winner> picked;
        for (Winner& candidate : candidates) {
            if (picked.size() == topPerGroup) {
                break;
            }
            const bool clone = std::ranges::any_of(
                picked, [&candidate](const Winner& p) {
                    return sameResults(candidate, p);
                });
            const bool diverse = std::ranges::all_of(
                picked, [&candidate](const Winner& p) {
                    return isDiverse(candidate, p);
                });
            if (clone || !diverse) {
                continue;
            }
            picked.push_back(std::move(candidate));
        }
        // groups is a std::map, so iteration (and therefore the output) is
        // already ordered by symbol then strategy name.
        winners.insert(winners.end(),
                       std::make_move_iterator(picked.begin()),
                       std::make_move_iterator(picked.end()));
    }
    return winners;
}

std::vector<Winner> selectTopWinners(
    const std::string& searchResponseBody,
    std::span<const std::string_view> activeStrategies,
    const std::size_t topPerGroup) {
    nlohmann::json response;
    try {
        response = nlohmann::json::parse(searchResponseBody);
    } catch (const std::exception& e) {
        backtest_log::error(std::string("liveWinners: unparseable _search response: ")
                            + e.what());
        return {};
    }
    GroupMap groups;
    collectCandidates(response, activeStrategies, groups);
    return pickWinners(groups, topPerGroup);
}

std::string buildWinnersQueryBody(const double minScore,
                                  const double maxDrawdownPercent,
                                  const double minCalmarScore,
                                  const std::string_view strategyName,
                                  const std::string_view symbol) {
    // Server-side eligibility: the score floor, the drawdown ceiling, the
    // calmar floor and the full-history window, narrowed to one
    // (strategy, symbol). LAST_MONTHS must equal the terminal rung's
    // exactly, but OFFSET_MONTHS is excluded when beyond the rung's (> 0)
    // instead of term-matched to it, because documents written before the
    // field existed carry no OFFSET_MONTHS at all yet ran offset-0 — the
    // same missing-means-default read RunConfiguration's from_json gives
    // them. The ceiling uses lte on results.maxDrawdownPercent and the
    // floor gte on results.calmarScore, so a document missing either field
    // does NOT match — fail-closed, matching the trade-lock doctrine.
    const nlohmann::json scoreFloor{
        {"range", {{"results.performanceScore", {{"gt", minScore}}}}}};
    const nlohmann::json drawdownCeiling{
        {"range",
         {{"results.maxDrawdownPercent", {{"lte", maxDrawdownPercent}}}}}};
    const nlohmann::json calmarFloor{
        {"range", {{"results.calmarScore", {{"gte", minCalmarScore}}}}}};
    const nlohmann::json fullWindow{
        {"term", {{"config.LAST_MONTHS", rolling::kFullHistory.lastMonths}}}};
    const nlohmann::json strategyTerm{
        {"term",
         {{"config.STRATEGY.TRADING_VARIABLES.STRATEGY.keyword",
           std::string{strategyName}}}}};
    // match, not a .keyword term: the analyzer tokenises a legacy
    // multi-symbol SYMBOLS ("EURUSD,USDJPY") so each symbol's query still
    // finds it; a whole-field term would silently drop those documents.
    const nlohmann::json symbolMatch{
        {"match", {{"config.SYMBOLS", std::string{symbol}}}}};
    const nlohmann::json offsetWindow{
        {"range",
         {{"config.OFFSET_MONTHS", {{"gt", rolling::kFullHistory.offsetMonths}}}}}};
    const nlohmann::json fullRunQuery{
        {"bool",
         {{"filter",
           nlohmann::json::array({scoreFloor, drawdownCeiling, calmarFloor,
                                  fullWindow, strategyTerm, symbolMatch})},
          {"must_not", nlohmann::json::array({offsetWindow})}}}};
    const nlohmann::json body{
        {"query", fullRunQuery},
        {"sort", nlohmann::json::array(
                     {{{"results.performanceScore", "desc"}}})},
        {"size", kMaxHits},
    };
    return body.dump();
}

std::vector<Winner> fetchWinners(
    const double minScore, const double maxDrawdownPercent,
    const double minCalmarScore, const std::size_t topPerGroup,
    std::span<const std::string_view> activeStrategies,
    std::span<const std::string_view> symbols) {
    GroupMap groups;
    const std::string winnersAlias =
        outcome_index::currentAlias(outcome_index::kWinnersBase);
    for (const std::string_view strategyName : activeStrategies) {
        for (const std::string_view symbol : symbols) {
            const std::string pair =
                std::string{strategyName} + "/" + std::string{symbol};
            std::string response;
            long httpStatus = 0;
            const int rc = elastic::searchIndex(
                winnersAlias,
                buildWinnersQueryBody(minScore, maxDrawdownPercent,
                                      minCalmarScore, strategyName, symbol),
                response, httpStatus);
            if (rc != 0) {
                // No fallback on a 4xx: retrying (say, without the sort
                // clause) would silently void the invariant that truncation
                // keeps the best-scoring docs, and mask a genuine query bug.
                // Log the body — for a 400 it carries Elasticsearch's reason
                // — and move on to the remaining pairs: searchIndex already
                // retried transient failures internally, so trading the
                // others beats trading none, and a persistent outage fails
                // every query and lands on the caller's fatal zero-winners
                // path anyway.
                backtest_log::error(
                    "liveWinners: " + winnersAlias + " _search for " + pair
                    + " failed (code " + std::to_string(rc) + ", HTTP "
                    + std::to_string(httpStatus) + "): "
                    + response.substr(0, 500));
                continue;
            }

            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(response);
            } catch (const std::exception& e) {
                backtest_log::error(
                    "liveWinners: unparseable _search response for " + pair
                    + ": " + e.what());
                continue;
            }

            // Loud per-pair truncation: if more docs qualify than the page
            // holds, say so instead of silently pretending full coverage.
            // With the sort in place the kept page is still that pair's
            // best.
            try {
                const auto total = parsed.at("hits").at("total").at("value")
                                       .get<std::size_t>();
                if (total > kMaxHits) {
                    backtest_log::error(
                        "liveWinners: " + std::to_string(total) + " " + pair
                        + " docs cleared minScore but only "
                        + std::to_string(kMaxHits) + " were fetched");
                }
            } catch (const std::exception&) {
                // hits.total is informational only; absence is not an error.
            }

            collectCandidates(parsed, activeStrategies, groups);
        }
    }
    return pickWinners(groups, topPerGroup);
}

}  // namespace live
