# Update and initialize the libpqxx submodule and any nested submodules it may have
# --init: Initialize submodules if they haven't been already
# --recursive: Update nested submodules recursively
git submodule update --init --recursive

# Navigate into the libpqxx submodule directory
cd ./external/libpqxx

# Create a build directory for out-of-source build
# -p ensures parent directories are created if they don't exist
mkdir -p build
cd ./build

# Generate build system files using CMake
# '..' points to the libpqxx root directory containing CMakeLists.txt
cmake .. 

# Configure the build with specific C++ compiler flags:
# -std=c++20: Use C++20 standard
# -O3: Enable maximum optimization
# --enable-silent-rules: Reduce build output verbosity
./configure CXXFLAGS="-std=c++20 -O3" --enable-silent-rules

# Compile libpqxx using generated build files
make