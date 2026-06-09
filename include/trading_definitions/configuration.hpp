// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <boost/decimal.hpp>
#include <nlohmann/json.hpp>
#include "trading_definitions/run_configuration.hpp"
#include "trading_definitions/strategy.hpp"
#include "utilities/decimal_json.hpp"

namespace trading_definitions {
// The risk fields (STARTING_BALANCE, MAX_LOSS_PERCENT, MAX_OPEN_TRADES,
// REPORT_FAILURES) mirror RunConfiguration — see the comments there.
struct Configuration {
  std::string RUN_ID;
  std::string SYMBOLS;
  int LAST_MONTHS;
  boost::decimal::decimal64_t STARTING_BALANCE{DEFAULT_STARTING_BALANCE};
  boost::decimal::decimal64_t MAX_LOSS_PERCENT{0};
  int MAX_OPEN_TRADES{0};
  bool REPORT_FAILURES{true};
  Strategy STRATEGY;
};

// Hand-written so the original fields stay strictly required while the risk
// fields fall back to the struct defaults — configs written before they
// existed still parse.
inline void to_json(nlohmann::json& j, const Configuration& c) {
  j = nlohmann::json{
      {"RUN_ID", c.RUN_ID},
      {"SYMBOLS", c.SYMBOLS},
      {"LAST_MONTHS", c.LAST_MONTHS},
      {"STARTING_BALANCE", c.STARTING_BALANCE},
      {"MAX_LOSS_PERCENT", c.MAX_LOSS_PERCENT},
      {"MAX_OPEN_TRADES", c.MAX_OPEN_TRADES},
      {"REPORT_FAILURES", c.REPORT_FAILURES},
      {"STRATEGY", c.STRATEGY},
  };
}

inline void from_json(const nlohmann::json& j, Configuration& c) {
  j.at("RUN_ID").get_to(c.RUN_ID);
  j.at("SYMBOLS").get_to(c.SYMBOLS);
  j.at("LAST_MONTHS").get_to(c.LAST_MONTHS);
  j.at("STRATEGY").get_to(c.STRATEGY);
  const Configuration defaults{};
  c.STARTING_BALANCE = j.value("STARTING_BALANCE", defaults.STARTING_BALANCE);
  c.MAX_LOSS_PERCENT = j.value("MAX_LOSS_PERCENT", defaults.MAX_LOSS_PERCENT);
  c.MAX_OPEN_TRADES = j.value("MAX_OPEN_TRADES", defaults.MAX_OPEN_TRADES);
  c.REPORT_FAILURES = j.value("REPORT_FAILURES", defaults.REPORT_FAILURES);
}

};
