#!/bin/bash
# This executes the run script

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Build the source code
source "$current_dir/clean.sh"
if ! source "$current_dir/build.sh"; then
    echo "Error: Build failed. Aborting."
    exit 1
fi

# Debug: Check if the executable exists
if [ -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Executable $EXECUTABLE_NAME found in $BUILD_DIR."
else
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR" # List the contents of the build directory for debugging
    exit 1
fi

if ! redis-cli -h localhost ping >/dev/null 2>&1; then
    echo "redis-server not reachable on localhost:6379 — skipping"
    exit 0
fi

json='{
  "RUN_ID": "UNIQUE_IDENTIFIER",
  "SYMBOLS": "EURUSD,AUDUSD",
  "LAST_MONTHS": 2,
  "STRATEGY": {
      "UUID": "",
      "TRADING_VARIABLES": {
          "STRATEGY": "RandomStrategy",
          "STOP_DISTANCE_IN_PIPS": "1.5",
          "LIMIT_DISTANCE_IN_PIPS": "1.5",
          "TRADING_SIZE": "1"
      },
      "OHLC_VARIABLES": [
          {
              "OHLC_COUNT": 60,
              "OHLC_MINUTES": 100
          }
      ],
      "STRATEGY_VARIABLES": {
        "OHLC_RSI_VARIABLES": {
            "RSI_LONG": 60,
            "RSI_SHORT": 40
        }
      }
  }
}'

if ! ./"$BUILD_DIR/$EXECUTABLE_NAME" load "$json"; then
    exit 1
fi

start_time=$(date +%s%N)
# Invoke the `run` subcommand: BacktestingEngine pops a Base64-encoded
# strategy off the Redis `strategy_queue` and executes it against the
# QuestDB host passed as the second argument (here, localhost).
./"$BUILD_DIR/$EXECUTABLE_NAME" run localhost
end_time=$(date +%s%N)
elapsed=$(( (end_time - start_time) / 1000000 ))
echo "Execution time: ${elapsed}ms"

