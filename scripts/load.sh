#!/bin/bash
# Builds the engine and runs the `load` subcommand per strategy: expands each
# sweep's parameter grid into keyed Redis payloads plus a run descriptor on
# BACKTESTING_QUEUE_RUN (see shared/utilities/queueKeys.hpp), and prepares the
# batch's weekly Elasticsearch outcome indices + -current aliases.

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
load_args=("$@")

# One batch label for every invocation below: the label names the weekly
# Elasticsearch indices (backtesting-results-2026-28, ...), and pinning it
# here keeps a load that straddles the Sunday-midnight-UTC ISO-week boundary
# from splitting one batch across two labels. Pre-set BACKTEST_BATCH wins —
# that is the re-run/test override.
export BACKTEST_BATCH="${BACKTEST_BATCH:-$(date -u +%G-%V)}"

# Strategies queued by a no-arg run; `random` is excluded — load it explicitly.
# keltnerFade retired 2026-28: 7 winners, all on the EURUSD control symbol the
# thesis said should do worst, zero on the six ranging crosses it targeted.
# Its sweep module remains — load it explicitly to re-test.
all_strategies=(ohlcBreakout fvg sessionRangeBreakout squeezeBreakout nyOpenRangeBreakout liquiditySweepReversal rangeVelocity)

if ! source "$current_dir/build.sh"; then
    echo "Error: Build failed. Aborting."
    exit 1
fi

if [ ! -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR"
    exit 1
fi

if ! redis-cli -h "$REDIS_HOST" ping >/dev/null 2>&1; then
    echo "redis-server not reachable on $REDIS_HOST:6379 — skipping"
    exit 0
fi

if [ ${#load_args[@]} -gt 0 ]; then
    exec ./"$BUILD_DIR/$EXECUTABLE_NAME" load "${load_args[@]}"
fi

i=0
for strategy in "${all_strategies[@]}"; do
    echo ""
    echo "=== [$((++i))/${#all_strategies[@]}] Loading strategy: $strategy ==="
    if ! ./"$BUILD_DIR/$EXECUTABLE_NAME" load "$strategy"; then
        echo "Error: load failed for '$strategy'. Aborting remaining strategies."
        exit 1
    fi
done
