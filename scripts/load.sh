#!/bin/bash
# Builds the engine and pushes the built-in strategy JSON
# (source/commands/loadCommand.cpp) onto the Redis `strategy_queue`.

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if ! source "$current_dir/build.sh"; then
    echo "Error: Build failed. Aborting."
    exit 1
fi

if [ ! -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR"
    exit 1
fi

if ! redis-cli -h localhost ping >/dev/null 2>&1; then
    echo "redis-server not reachable on localhost:6379 — aborting"
    exit 1
fi

exec ./"$BUILD_DIR/$EXECUTABLE_NAME" load
