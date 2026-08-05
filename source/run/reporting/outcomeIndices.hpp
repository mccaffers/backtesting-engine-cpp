// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <array>
#include <ctime>
#include <string>
#include <string_view>

// Naming for the outcome indices in Elasticsearch: `backtesting-` prefix,
// dashes throughout, and a per-week batch suffix so each weekly sweep lands in
// its own index ("backtesting-results-2026-28") while the history of earlier
// weeks stays untouched and searchable via the backtesting-* pattern. The
// batch label is minted ONCE at load time (currentBatchLabel) and rides inside
// every run configuration — never recomputed at write time — so a run's
// documents always target the same index no matter when the flusher delivers
// them: the deterministic doc _id (RUN_ID:UUID) keeps retries and dead-letter
// replays idempotent only within a stable index, and the ladder's terminal
// {9,0} run, drained days after the seed, still lands in the seed week.
//
// Each base also has a rolling "-current" alias, atomically repointed at the
// newest weekly index by the load command (elastic::repointAlias) — readers of
// "this week's run" (live winner selection, dashboards) follow the alias and
// never race the calendar. The alias name must differ from every concrete
// index name, which the batch suffix guarantees.
//
// The legacy trading_* indices are a frozen archive: an empty batch label
// (payloads queued before the field existed, hand-run configs) falls back to
// the bare base name, not to the old names.
namespace outcome_index {

// Gate-cleared screening runs — every ladder rung EXCEPT the terminal
// full-history window (those go to kWinnersBase, see elasticClient).
inline constexpr std::string_view kResultsBase = "backtesting-results";
// Gate-cleared full-history ({9,0}) runs — exactly the population live's
// winner selection trades, kept apart so live boots against a small index.
inline constexpr std::string_view kWinnersBase = "backtesting-winners";
// Terminal record for EVERY run (completed / underperformed / cut off).
inline constexpr std::string_view kFinalBase = "backtesting-final";
// Runs cut off by the loss limit (when REPORT_FAILURES).
inline constexpr std::string_view kFailuresBase = "backtesting-failures";
// One document per closed trade (opt-in, high volume) — renamed with the
// convention but deliberately NOT rotated weekly.
inline constexpr std::string_view kTradesIndex = "backtesting-trades";
// One aggregate document per experiment x symbol group (the `analysis`
// worker). Weekly + -current alias like the outcome bases, but deliberately
// NOT in kWeeklyBases: `load` must not create empty experiment indices —
// `experiments` prepares this base itself.
inline constexpr std::string_view kExperimentsBase = "backtesting-experiments";

// Every base the load command prepares each week (index + -current alias).
inline constexpr std::array<std::string_view, 4> kWeeklyBases{
    kResultsBase, kWinnersBase, kFinalBase, kFailuresBase};

// "backtesting-results-2026-28"; the bare base when `batch` is empty, so
// pre-batch configs keep writing somewhere sensible without alias admin.
std::string weeklyIndex(std::string_view base, std::string_view batch);

// "backtesting-results-current" — the rolling alias for the newest batch.
std::string currentAlias(std::string_view base);

// ISO-8601 year and week of `utc`, dash-separated and zero-padded
// ("2026-28"), so lexical order is chronological order. The ISO year can
// differ from the calendar year around New Year — that is the point: every
// day of one ISO week maps to one label.
std::string isoWeekLabel(std::time_t utc);

// The batch label a load mints: $BACKTEST_BATCH when set and non-empty
// (re-runs, tests, pinning a multi-invocation load to one label), otherwise
// the current UTC ISO week.
std::string currentBatchLabel();

}  // namespace outcome_index
