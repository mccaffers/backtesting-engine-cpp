#!/bin/bash
# This file sets the environment variables for execution

# Get the current execution directory
EXEC_DIR="$(pwd)"

# Initialize a flag for demo mode
USE_DEMO=false

# Parse command line arguments
for arg in "$@"
do
    case $arg in
        --demo)
        USE_DEMO=true
        shift # Remove --demo from processing
        ;;
    esac
done

# Determine which environment file to use
if [ "$USE_DEMO" = true ] || [ ! -f "$EXEC_DIR/.env/prod.env" ]; then
    echo "Using Demo Environment Variables"
    ENV_FILE="$EXEC_DIR/.env/demo.env"
else
    echo "Using Production Environment Variables"
    ENV_FILE="$EXEC_DIR/.env/prod.env"
fi

# You can now use $ENV_FILE which contains the full path to the environment file
echo "Full path to env file: $ENV_FILE"

# Now import the selected .env file
set -a
while IFS= read -r line || [[ -n "$line" ]]; do
    # Skip comments and empty lines
    [[ $line =~ ^[[:space:]]*#.*$ ]] && continue
    [[ -z "$line" ]] && continue
    # Export the variable
    export "$line"
done < "$ENV_FILE"
set +a