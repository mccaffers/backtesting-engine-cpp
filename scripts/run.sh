#!/bin/bash
# This executes the run script

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Build the source code
source $current_dir/clean.sh
source $current_dir/build.sh
source $current_dir/environment.sh

# Debug: Check if the executable exists
if [ -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Executable $EXECUTABLE_NAME found in $BUILD_DIR."
else
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR" # List the contents of the build directory for debugging
    exit 1
fi


# Step 6: Run the tests for now (/executable) from the root directory
./"$BUILD_DIR/$EXECUTABLE_NAME"

