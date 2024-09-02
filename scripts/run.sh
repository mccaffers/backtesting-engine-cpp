#!/bin/bash

# Variables
BUILD_DIR="build"
EXECUTABLE_NAME="MyProgram"
export DATA_FILE_PATH="resources/EURUSD/2020.csv"

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

# Debug: Check if the executable exists
if [ -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Executable $EXECUTABLE_NAME found in $BUILD_DIR."
else
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR" # List the contents of the build directory for debugging
    exit 1
fi

# Step 6: Run the tests for now (/executable) from the root directory
./"$BUILD_DIR/$EXECUTABLE_NAME"Tests
# ./"$BUILD_DIR/$EXECUTABLE_NAME"

# # # Step 7: Clean up by removing the build directory
rm -rf "$BUILD_DIR"

# # Optional: Print a success message
echo "Build, run, and cleanup complete!"