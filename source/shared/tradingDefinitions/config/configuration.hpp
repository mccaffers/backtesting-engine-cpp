// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <boost/decimal.hpp>
#include <nlohmann/json.hpp>
#include "shared/tradingDefinitions/config/runConfiguration.hpp"
#include "shared/tradingDefinitions/strategyConfig.hpp"
#include "shared/utilities/decimalJson.hpp"

// Intentionally a plain header, NOT a .cppm module. Every JSON-serializable
// struct in this repo stays a GMF-includable header and is #include'd into the
// global module fragment of its consumers (next to <nlohmann/json.hpp>), because
// nlohmann's ADL-based to_json/from_json do not resolve reliably across a module
// import boundary. See source/.../*.cppm GMFs and the module-migration notes.
namespace tradingDefinitions {
// The window fields (LAST_MONTHS, OFFSET_MONTHS), batch identity (BATCH,
// EXECUTION_TS) and risk/simulation fields (STARTING_BALANCE,
// MAX_LOSS_PERCENT, MAX_OPEN_TRADES, MAX_TRADES_PER_MINUTE, REPORT_FAILURES,
// PEAK_HOURS_ONLY, ENTRY_SLIPPAGE_TENTH_PIPS) mirror RunConfiguration — see
// the comments there.
struct Configuration {
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
  StrategyConfig STRATEGY;
};

// Hand-written so the original fields stay strictly required while the risk
// fields fall back to the struct defaults — configs written before they
// existed still parse.
inline void to_json(nlohmann::json& j, const Configuration& c) {
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
      {"STRATEGY", c.STRATEGY},
  };
}

inline void from_json(const nlohmann::json& j, Configuration& c) {
  j.at("RUN_ID").get_to(c.RUN_ID);
  j.at("SYMBOLS").get_to(c.SYMBOLS);
  j.at("LAST_MONTHS").get_to(c.LAST_MONTHS);
  j.at("STRATEGY").get_to(c.STRATEGY);
  const Configuration defaults{};
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
