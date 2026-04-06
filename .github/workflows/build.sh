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

# Expose paths so CMake finds libpq
export PATH="$(brew --prefix libpq)/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:$PKG_CONFIG_PATH"
export PostgreSQL_ROOT="$(brew --prefix libpq)"

# 1. Generate build files (Passing your CXX flags directly to CMake instead of configure)
cmake .. -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release 

# 2. Compile libpqxx
make