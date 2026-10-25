#!/bin/bash
# This executes the analysis script — run.sh's twin for the experiment queue:
# drains BACKTESTING_QUEUE_EXPERIMENT_RUN and evaluates each experiment chain
# against the QuestDB tick stream, reporting one aggregate document per
# experiment x symbol group to the weekly backtesting-experiments index.

required_vars=(ELASTIC_HOST ELASTIC_USER ELASTIC_USER_PASSWORD REDIS_HOST)
missing=()
for var in "${required_vars[@]}"; do
    [[ -z "${!var}" ]] && missing+=("$var")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Error: missing required environment variables: ${missing[*]}"
    echo "eg. infisical run -- sh ./scripts/analysis.sh (if using infisical)"
    echo "or ELASTIC_HOST=localhost ELASTIC_USER=elastic ELASTIC_USER_PASSWORD=password REDIS_HOST=localhost sh scripts/analysis.sh"
    exit 1
fi

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

if ! redis-cli -h "$REDIS_HOST" ping >/dev/null 2>&1; then
    echo "redis-server not reachable on $REDIS_HOST:6379 — skipping"
    exit 0
fi

start_time=$(date +%s%N)
# Invoke the `analysis` subcommand: BacktestingEngine peeks a run descriptor
# off the experiment Redis queue and evaluates its experiment chains against
# the QuestDB host passed as the second argument (here, localhost). Daemon —
# Ctrl+C to stop (analysis has no shared-memory control channel; that is
# run-only).
./"$BUILD_DIR/$EXECUTABLE_NAME" analysis localhost
end_time=$(date +%s%N)
elapsed=$(( (end_time - start_time) / 1000000 ))
echo "Execution time: ${elapsed}ms"
