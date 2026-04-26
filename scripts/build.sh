#!/bin/bash

git submodule update --init --recursive

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
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) \
  -DSKIP_BUILD_TEST=ON

# Step 4: Compile the project
cmake --build .

# Step 5: Navigate back to the root directory
cd ..
