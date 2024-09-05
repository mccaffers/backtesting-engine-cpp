#!/bin/bash
current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Build the source code
source $current_dir/build.sh

# Step 6: Run the tests for now (/executable) from the root directory
./"$BUILD_DIR/$EXECUTABLE_NAME"Tests
