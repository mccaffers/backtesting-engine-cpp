// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <boost/decimal.hpp>
#include <nlohmann/json.hpp>
#include "shared/utilities/decimalJson.hpp"

// Intentionally a plain header, NOT a .cppm module — see configuration.hpp for
// why JSON-serializable structs stay GMF-includable headers in this repo.
namespace tradingDefinitions {

// Single source of truth for the default account balance. Configuration and
// trading::RiskLimits reference this rather than repeating the literal.
inline constexpr boost::decimal::decimal64_t DEFAULT_STARTING_BALANCE{10000};

// Run-level descriptor: the unit of QuestDB tick data shared by every strategy
// in a sweep. Carried on BACKTESTING_QUEUE_RUN and linked to its strategies via
// RUN_ID (see queueKeys.hpp). The runner reassembles a full Configuration from
// this plus each popped Strategy.
//
// The risk settings apply to every strategy in the sweep:
//  - STARTING_BALANCE / MAX_LOSS_PERCENT: each run starts from
//    STARTING_BALANCE and is cut off early (open trades liquidated) once its
//    equity loss reaches MAX_LOSS_PERCENT of it. <= 0 disables the cutoff.
//  - MAX_OPEN_TRADES: cap on simultaneously open positions per run; entries
//    are skipped while at the cap. <= 0 means unlimited.
//  - MAX_TRADES_PER_MINUTE: cap on trade entries within any sliding 60-second
//    window of TICK time (backtests replay history, so wall clock would be
//    meaningless); entries are skipped while at the cap. <= 0 means
//    unlimited. Defaults to 60 — unlike the other risk knobs this one is ON
//    by default, as a runaway-strategy brake.
//  - REPORT_FAILURES: when false, runs cut off by the loss limit are NOT
//    reported to Elasticsearch (silencer for large sweeps where liquidated
//    runs are expected noise). Completed runs always report.
//  - PEAK_HOURS_ONLY: when true, entries are allowed only inside the
//    symbol's peak session window (market_hours::tradePermitted — weekend
//    block plus per-session hours). Exits are never gated. Defaults to
//    false so payloads and winners written before the field existed keep
//    their behaviour.
//  - ENTRY_SLIPPAGE_TENTH_PIPS: slippage stress toggle, in TENTHS of a pip
//    (3 = 0.3 pip). Every backtest entry fills that much AGAINST the trade
//    (LONG above the ask, SHORT below the bid); SL/TP anchors stay on the
//    raw tick. 0 (the default) is off, so existing payloads and unstressed
//    sweeps are unchanged. Backtest-only — live fills belong to the broker.
//
// The tick window is LAST_MONTHS long and ends OFFSET_MONTHS before now, so
// OFFSET_MONTHS = 0 (the default) tests the most recent data while e.g.
// LAST_MONTHS = 6, OFFSET_MONTHS = 12 replays the 6-month window that ended a
// year ago — rolling historical windows without touching the data.
//
// BATCH / EXECUTION_TS are the batch identity minted once per load (see
// outcomeIndices.hpp): BATCH is the ISO week label ("2026-28") that names the
// weekly Elasticsearch outcome indices, EXECUTION_TS the seed wall clock the
// whole batch shares. They ride through Redis and every rolling-window rung
// unchanged so a run's documents always target the seed week's index. Empty
// (payloads written before the fields existed, hand-run configs) means
// unsuffixed index names and no batch metadata on the documents.
struct RunConfiguration {
  std::string RUN_ID;
  std::string SYMBOLS;
  std::string BATCH;
  std::string EXECUTION_TS;
  int LAST_MONTHS = 0;
  int OFFSET_MONTHS = 0;
  boost::decimal::decimal64_t STARTING_BALANCE{DEFAULT_STARTING_BALANCE};
  boost::decimal::decimal64_t MAX_LOSS_PERCENT{0};
  int MAX_OPEN_TRADES{0};
  int MAX_TRADES_PER_MINUTE{60};
  bool REPORT_FAILURES{true};
  bool PEAK_HOURS_ONLY{false};
  int ENTRY_SLIPPAGE_TENTH_PIPS{0};
};

// Hand-written (rather than NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE) so the
// original fields stay strictly required while the risk fields fall back to
// the struct defaults — payloads written before they existed still parse.
inline void to_json(nlohmann::json& j, const RunConfiguration& c) {
  j = nlohmann::json{
      {"RUN_ID", c.RUN_ID},
      {"SYMBOLS", c.SYMBOLS},
      {"BATCH", c.BATCH},
      {"EXECUTION_TS", c.EXECUTION_TS},
      {"LAST_MONTHS", c.LAST_MONTHS},
      {"OFFSET_MONTHS", c.OFFSET_MONTHS},
      {"STARTING_BALANCE", c.STARTING_BALANCE},
      {"MAX_LOSS_PERCENT", c.MAX_LOSS_PERCENT},
      {"MAX_OPEN_TRADES", c.MAX_OPEN_TRADES},
      {"MAX_TRADES_PER_MINUTE", c.MAX_TRADES_PER_MINUTE},
      {"REPORT_FAILURES", c.REPORT_FAILURES},
      {"PEAK_HOURS_ONLY", c.PEAK_HOURS_ONLY},
      {"ENTRY_SLIPPAGE_TENTH_PIPS", c.ENTRY_SLIPPAGE_TENTH_PIPS},
  };
}

inline void from_json(const nlohmann::json& j, RunConfiguration& c) {
  j.at("RUN_ID").get_to(c.RUN_ID);
  j.at("SYMBOLS").get_to(c.SYMBOLS);
  j.at("LAST_MONTHS").get_to(c.LAST_MONTHS);
  const RunConfiguration defaults{};
  c.BATCH = j.value("BATCH", defaults.BATCH);
  c.EXECUTION_TS = j.value("EXECUTION_TS", defaults.EXECUTION_TS);
  c.OFFSET_MONTHS = j.value("OFFSET_MONTHS", defaults.OFFSET_MONTHS);
  c.STARTING_BALANCE = j.value("STARTING_BALANCE", defaults.STARTING_BALANCE);
  c.MAX_LOSS_PERCENT = j.value("MAX_LOSS_PERCENT", defaults.MAX_LOSS_PERCENT);
  c.MAX_OPEN_TRADES = j.value("MAX_OPEN_TRADES", defaults.MAX_OPEN_TRADES);
  c.MAX_TRADES_PER_MINUTE =
      j.value("MAX_TRADES_PER_MINUTE", defaults.MAX_TRADES_PER_MINUTE);
  c.REPORT_FAILURES = j.value("REPORT_FAILURES", defaults.REPORT_FAILURES);
  c.PEAK_HOURS_ONLY = j.value("PEAK_HOURS_ONLY", defaults.PEAK_HOURS_ONLY);
  c.ENTRY_SLIPPAGE_TENTH_PIPS =
      j.value("ENTRY_SLIPPAGE_TENTH_PIPS", defaults.ENTRY_SLIPPAGE_TENTH_PIPS);
}

};
