#!/bin/bash
# This executes the run script

current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Build the source code
source $current_dir/environment.sh
source $current_dir/clean.sh
source $current_dir/build.sh

# Debug: Check if the executable exists
if [ -f "$BUILD_DIR/$EXECUTABLE_NAME" ]; then
    echo "Executable $EXECUTABLE_NAME found in $BUILD_DIR."
else
    echo "Error: Executable $EXECUTABLE_NAME not found in $BUILD_DIR."
    ls -la "$BUILD_DIR" # List the contents of the build directory for debugging
    exit 1
fi

# Step 6: Run the tests for now (/executable) from the root directory
# Passing two arguements, the destination of the QuestDB and the Strategy JSON (in base64)
./"$BUILD_DIR/$EXECUTABLE_NAME" localhost ewogICJSVU5fSUQiOiAiaWQiLAogICJTWU1CT0xTIjogIkVVUlVTRCIsCiAgIkxBU1RfTU9OVEhTIjogNiwKICAiU1RSQVRFR1kiOiB7CiAgICAgICJVVUlEIjogIiIsCiAgICAgICJUUkFESU5HX1ZBUklBQkxFUyI6IHsKICAgICAgICAgICJTVFJBVEVHWSI6ICJPSExDX1JTSSIsCiAgICAgICAgICAiU1RPUF9ESVNUQU5DRV9JTl9QSVBTIjogMSwKICAgICAgICAgICJMSU1JVF9ESVNUQU5DRV9JTl9QSVBTIjogMSwKICAgICAgICAgICJUUkFESU5HX1NJWkUiOiAxCiAgICAgIH0sCiAgICAgICJPSExDX1ZBUklBQkxFUyI6IFsKICAgICAgICAgIHsKICAgICAgICAgICAgICAiT0hMQ19DT1VOVCI6IDYwLAogICAgICAgICAgICAgICJPSExDX01JTlVURVMiOiAxMDAKICAgICAgICAgIH0KICAgICAgXSwKICAgICAgIk9ITENfUlNJX1ZBUklBQkxFUyI6IHsKICAgICAgICAgICJSU0lfTE9ORyI6IDYwLAogICAgICAgICAgIlJTSV9TSE9SVCI6IDQwCiAgICAgIH0KICB9Cn0K

