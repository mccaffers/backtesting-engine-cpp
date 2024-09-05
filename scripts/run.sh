#!/bin/bash
current_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

clean_flag=false

# Loop through all arguments
for arg in "$@"
do
    case $arg in
        --clean)
        clean_flag=true
        shift # Remove --clean from processing
        ;;
        *)
        # Unknown option
        ;;
    esac
done

if [ "$clean_flag" = true ] ; then
    echo "Clean flag is set. Performing cleanup..."
    source $current_dir/clean.sh
    # Add your cleanup code here
else
    echo "Clean flag is not set. Skipping cleanup."
fi

# Build the source code
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
./"$BUILD_DIR/$EXECUTABLE_NAME"

