// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <boost/decimal.hpp>
#include <nlohmann/json.hpp>
#include "utilities/decimal_json.hpp"

namespace trading_definitions {

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
//  - REPORT_FAILURES: when false, runs cut off by the loss limit are NOT
//    reported to Elasticsearch (silencer for large sweeps where liquidated
//    runs are expected noise). Completed runs always report.
struct RunConfiguration {
  std::string RUN_ID;
  std::string SYMBOLS;
  int LAST_MONTHS;
  boost::decimal::decimal64_t STARTING_BALANCE{DEFAULT_STARTING_BALANCE};
  boost::decimal::decimal64_t MAX_LOSS_PERCENT{0};
  int MAX_OPEN_TRADES{0};
  bool REPORT_FAILURES{true};
};

// Hand-written (rather than NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE) so the
// original fields stay strictly required while the risk fields fall back to
// the struct defaults — payloads written before they existed still parse.
inline void to_json(nlohmann::json& j, const RunConfiguration& c) {
  j = nlohmann::json{
      {"RUN_ID", c.RUN_ID},
      {"SYMBOLS", c.SYMBOLS},
      {"LAST_MONTHS", c.LAST_MONTHS},
      {"STARTING_BALANCE", c.STARTING_BALANCE},
      {"MAX_LOSS_PERCENT", c.MAX_LOSS_PERCENT},
      {"MAX_OPEN_TRADES", c.MAX_OPEN_TRADES},
      {"REPORT_FAILURES", c.REPORT_FAILURES},
  };
}

inline void from_json(const nlohmann::json& j, RunConfiguration& c) {
  j.at("RUN_ID").get_to(c.RUN_ID);
  j.at("SYMBOLS").get_to(c.SYMBOLS);
  j.at("LAST_MONTHS").get_to(c.LAST_MONTHS);
  const RunConfiguration defaults{};
  c.STARTING_BALANCE = j.value("STARTING_BALANCE", defaults.STARTING_BALANCE);
  c.MAX_LOSS_PERCENT = j.value("MAX_LOSS_PERCENT", defaults.MAX_LOSS_PERCENT);
  c.MAX_OPEN_TRADES = j.value("MAX_OPEN_TRADES", defaults.MAX_OPEN_TRADES);
  c.REPORT_FAILURES = j.value("REPORT_FAILURES", defaults.REPORT_FAILURES);
}

};
