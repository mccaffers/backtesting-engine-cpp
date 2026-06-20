// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "shared/utilities/jsonParser.hpp"
#include "shared/utilities/base64.hpp"

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
          "STOP_DISTANCE_IN_PIPS": "1",
          "LIMIT_DISTANCE_IN_PIPS": "1",
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
}
