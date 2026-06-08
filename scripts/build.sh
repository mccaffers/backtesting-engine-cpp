#!/bin/bash

git submodule update --init --recursive

if [ -z "$(ls -A ./external/boost-decimal 2>/dev/null)" ] || [ -z "$(ls -A ./external/libpqxx 2>/dev/null)" ]; then
    ./scripts/build_dep.sh
fi

BUILD_DIR="build"
# Variables
EXECUTABLE_NAME="BacktestingEngine"

# Step 1: Create a build directory if it doesn't exist
if [ ! -d "$BUILD_DIR" ]; then
    mkdir "$BUILD_DIR"
fi

# Step 2: Navigate to the build directory
cd "$BUILD_DIR" || exit

# Expose paths so CMake finds libpq
if command -v brew &>/dev/null; then
    export PATH="$(brew --prefix libpq)/bin:$PATH"
    export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:$PKG_CONFIG_PATH"
    export PostgreSQL_ROOT="$(brew --prefix libpq)"
fi

# 1. Generate build files (Passing your CXX flags directly to CMake instead of configure)
cmake .. \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_BUILD_TYPE=Release \
  -DSKIP_BUILD_TEST=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Step 4: Compile the project
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake --build . --parallel "$JOBS"

# Step 5: Navigate back to the root directory
cd ..
