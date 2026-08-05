// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "shared/utilities/jsonParser.hpp"
#include "shared/utilities/base64.hpp"
#include "shared/tradingDefinitions/strategyConfig.hpp"
#include "shared/tradingDefinitions/variables/tradingVariables.hpp"

TEST_CASE("JsonParser parses a valid base64 configuration", "[jsonParser]") {
    // A sample configuration JSON with all required fields.
    std::string validJson = R"({
  "RUN_ID": "UNIQUE_IDENTIFER",
  "SYMBOLS": "EURUSD",
  "LAST_MONTHS": 6,
  "STRATEGY": {
      "UUID": "",
      "TRADING_VARIABLES": {
          "STRATEGY": "RandomStrategy",
          "STOP_DISTANCE_IN_ATR": "1",
          "LIMIT_DISTANCE_IN_ATR": "1",
          "TRADING_SIZE": "1"
      },
      "OHLC_VARIABLES": [
          {
              "OHLC_COUNT": 60,
              "OHLC_MINUTES": 100
          }
      ],
      "STRATEGY_VARIABLES" : {
        "OHLC_RSI_VARIABLES": {
            "RSI_LONG": 60,
            "RSI_SHORT": 40
        }
      }
  }
})";

    std::string base64Input = Base64::b64encode(validJson);

    tradingDefinitions::Configuration result =
        JsonParser::parseConfigurationFromBase64(base64Input);
    INFO("Parsing should succeed with valid JSON");
    CHECK_FALSE(result.RUN_ID.empty());
    // Payloads written before the slippage stress toggle existed parse with
    // it off — 0.0 pip of adverse entry slippage.
    CHECK(result.ENTRY_SLIPPAGE_TENTH_PIPS == 0);
    // Payloads written before the weekly batch identity existed parse with
    // it empty — unsuffixed legacy index routing, no batch metadata.
    CHECK(result.BATCH.empty());
    CHECK(result.EXECUTION_TS.empty());
    // Likewise for payloads written before range bars existed: no
    // RANGE_VARIABLES key parses as "builds no range bars", never a parse
    // failure — the most important regression in the range-bar plumbing
    // (a required key would silently drop every ES winner and Redis payload).
    CHECK(result.STRATEGY.RANGE_VARIABLES.empty());
}

TEST_CASE("StrategyConfig round-trips RANGE_VARIABLES when present",
          "[jsonParser]") {
    nlohmann::json j = {
        {"UUID", "u-range"},
        {"TRADING_VARIABLES",
         {{"STRATEGY", "RandomStrategy"},
          {"STOP_DISTANCE_IN_ATR", "1"},
          {"LIMIT_DISTANCE_IN_ATR", "1"},
          {"TRADING_SIZE", "1"}}},
        {"OHLC_VARIABLES", nlohmann::json::array()},
        {"RANGE_VARIABLES",
         {{{"RANGE_ATR_TICK_WINDOW", 5000},
           {"RANGE_ATR_PERCENT", 40},
           {"RANGE_COUNT", 50}}}},
        {"STRATEGY_VARIABLES", nlohmann::json::object()},
    };

    const auto config = j.get<tradingDefinitions::StrategyConfig>();
    REQUIRE(config.RANGE_VARIABLES.size() == 1);
    CHECK(config.RANGE_VARIABLES[0].RANGE_ATR_TICK_WINDOW == 5000);
    CHECK(config.RANGE_VARIABLES[0].RANGE_ATR_PERCENT == 40);
    CHECK(config.RANGE_VARIABLES[0].RANGE_COUNT == 50);

    // to_json always writes the key, so new documents round-trip exactly.
    const nlohmann::json out = config;
    CHECK(out.at("RANGE_VARIABLES") == j.at("RANGE_VARIABLES"));
}

// Zero is the "unused" sentinel; negatives are poison-pill payloads that must
// fail the parse before reaching the bar builder — same doctrine as
// OHLCVariables.
TEST_CASE("RangeBarVariables rejects negative fields", "[jsonParser]") {
    nlohmann::json j = {{"RANGE_ATR_TICK_WINDOW", -1},
                        {"RANGE_ATR_PERCENT", 40},
                        {"RANGE_COUNT", 50}};
    CHECK_THROWS_AS(j.get<tradingDefinitions::RangeBarVariables>(),
                    std::invalid_argument);

    j["RANGE_ATR_TICK_WINDOW"] = 5000;
    j["RANGE_COUNT"] = -50;
    CHECK_THROWS_AS(j.get<tradingDefinitions::RangeBarVariables>(),
                    std::invalid_argument);

    j["RANGE_COUNT"] = 50;  // sane values still parse
    CHECK(j.get<tradingDefinitions::RangeBarVariables>().RANGE_ATR_TICK_WINDOW ==
          5000);
}

// readIntField reads pip distances / size through double: the old unchecked
// lround->int32 cast silently wrapped huge values into garbage (possibly
// negative) integers, and lround on "nan" is UB. Both must reject the config
// instead — a rejected payload is a contained poison pill, a wrapped size is a
// silently wrong backtest.
TEST_CASE("TradingVariables rejects out-of-range integer fields", "[jsonParser]") {
    nlohmann::json j = {
        {"STRATEGY", "RandomStrategy"},
        {"STOP_DISTANCE_IN_ATR", "10"},
        {"LIMIT_DISTANCE_IN_ATR", "10"},
        {"TRADING_SIZE", "99999999999"},   // wraps int32 under the old cast
    };
    CHECK_THROWS_AS(j.get<tradingDefinitions::TradingVariables>(),
                    std::invalid_argument);

    j["TRADING_SIZE"] = "nan";             // lround(NaN) is UB
    CHECK_THROWS_AS(j.get<tradingDefinitions::TradingVariables>(),
                    std::invalid_argument);

    j["TRADING_SIZE"] = "2";               // sane value still parses
    CHECK(j.get<tradingDefinitions::TradingVariables>().TRADING_SIZE == 2);
}
