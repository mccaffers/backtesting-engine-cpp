# Variables
BUILD_DIR="build"
EXECUTABLE_NAME="MyProgram"

# Get the current execution directory
EXEC_DIR="$(pwd)"

# Check for prod.env in the .env directory relative to the current execution directory
if [ -f "$EXEC_DIR/.env/prod.env" ]; then
    echo "Using Production Environment Variables"
    ENV_FILE="$EXEC_DIR/.env/prod.env"
else
    echo "Using Demo Environment Variables"
    ENV_FILE="$EXEC_DIR/.env/demo.env"
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