#!/bin/bash
# Builds the engine and runs the `experiments` subcommand: expands the given
# experiment sweep's parameter grid into keyed Redis payloads plus a run
# descriptor on BACKTESTING_QUEUE_EXPERIMENT_RUN (see
# shared/utilities/queueKeys.hpp), and prepares the batch's weekly
# backtesting-experiments Elasticsearch index + -current alias.
#
# Usage mirrors load.sh:
#   sh scripts/experiments.sh dipRecovery    # queue one named sweep
#   sh scripts/experiments.sh                # queue the default sweep set
#
# Interactive: the engine prints the total sweep size and waits for Enter on
# stdin before touching Redis — run it from a TTY, or pipe the newline for
# non-interactive use (closed stdin counts as a decline and aborts).

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
experiments_args=("$@")

# One batch label for every invocation below: the label names the weekly
# backtesting-experiments index (backtesting-experiments-2026-28, ...), and
# pinning it here keeps a load that straddles the Sunday-midnight-UTC ISO-week
# boundary from splitting one batch across two labels. Pre-set BACKTEST_BATCH
# wins — that is the re-run/test override.
export BACKTEST_BATCH="${BACKTEST_BATCH:-$(date -u +%G-%V)}"

# Sweeps queued by a no-arg run.
all_sweeps=(dipRecovery rangeRelativeDipRecovery)

if ! source "$current_dir/build.sh"; then
    echo "Error: Build failed. Aborting."
    exit 1
fi

if [ ! -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR"
    exit 1
fi

if ! redis-cli -h "${REDIS_HOST:-127.0.0.1}" ping >/dev/null 2>&1; then
    echo "redis-server not reachable on ${REDIS_HOST:-127.0.0.1}:6379 — skipping"
    exit 0
fi

if [ ${#experiments_args[@]} -gt 0 ]; then
    exec ./"$BUILD_DIR/$EXECUTABLE_NAME" experiments "${experiments_args[@]}"
fi

i=0
for sweep in "${all_sweeps[@]}"; do
    echo ""
    echo "=== [$((++i))/${#all_sweeps[@]}] Loading experiment sweep: $sweep ==="
    if ! ./"$BUILD_DIR/$EXECUTABLE_NAME" experiments "$sweep"; then
        echo "Error: experiments failed for '$sweep'. Aborting remaining sweeps."
        exit 1
    fi
done
