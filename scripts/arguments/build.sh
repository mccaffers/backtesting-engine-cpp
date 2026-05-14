json='{
  "RUN_ID": "UNIQUE_IDENTIFER",
  "SYMBOLS": "EURUSD",
  "LAST_MONTHS": 6,
  "STRATEGY": {
      "UUID": "",
      "TRADING_VARIABLES": {
          "STRATEGY": "RandomStrategy",
          "STOP_DISTANCE_IN_PIPS": 1,
          "LIMIT_DISTANCE_IN_PIPS": 1,
          "TRADING_SIZE": 1
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
}'

echo "$json" | base64
