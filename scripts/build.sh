#!/bin/bash

# Variables
EXECUTABLE_NAME="BacktestingEngine"

# Step 1: Create a build directory if it doesn't exist
if [ ! -d "$BUILD_DIR" ]; then
    mkdir "$BUILD_DIR"
fi

# Step 2: Navigate to the build directory
cd "$BUILD_DIR" || exit

# Step 3: Run CMake to configure the project
cmake ..

# Step 4: Compile the project
cmake --build .

# Step 5: Navigate back to the root directory
cd ..