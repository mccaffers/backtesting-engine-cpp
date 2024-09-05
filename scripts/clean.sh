#!/bin/bash

BUILD_DIR="build"
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
    rm -rf "$BUILD_DIR"
    # Add your cleanup code here
else
    echo "Clean flag is not set. Skipping cleanup."
fi