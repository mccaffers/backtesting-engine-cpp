#!/bin/bash
# This executes the run script

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Build the source code
# source $current_dir/environment.sh - no longer necessary
source $current_dir/clean.sh
source $current_dir/build.sh

# Debug: Check if the executable exists
if [ -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Executable $EXECUTABLE_NAME found in $BUILD_DIR."
else
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR" # List the contents of the build directory for debugging
    exit 1
fi

json='{
  "RUN_ID": "UNIQUE_IDENTIFER",
  "SYMBOLS": "EURUSD",
  "LAST_MONTHS": 6,
  "STRATEGY": {
      "UUID": "",
      "TRADING_VARIABLES": {
          "STRATEGY": "OHLC_RSI",
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

output=$(echo "$json" | base64)


# Step 6: Run the tests for now (/executable) from the root directory
# Passing two arguements, the destination of the QuestDB and the Strategy JSON (in base64)
./"$BUILD_DIR/$EXECUTABLE_NAME" 100.85.44.114 "$output"

