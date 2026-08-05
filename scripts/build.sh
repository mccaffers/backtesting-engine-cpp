#!/bin/bash

git submodule update --init --recursive

# An AWS SDK install produced by the old default-toolchain build (GCC/
# libstdc++ on Linux) satisfies the AWSSDKConfig.cmake gate below but fails
# the engine's libc++ link with undefined std::__cxx11 references. Delete it
# here so the gate re-runs build_dep.sh, which rebuilds with clang/libc++.
# (build_dep.sh has the same heal, but it only helps if it actually runs.)
if ! command -v brew &>/dev/null; then
    AWS_CORE_LIB="./external/aws-install/lib/libaws-cpp-sdk-core.a"
    [ -f "$AWS_CORE_LIB" ] || AWS_CORE_LIB="./external/aws-install/lib64/libaws-cpp-sdk-core.a"
    if [ -f "$AWS_CORE_LIB" ] && nm "$AWS_CORE_LIB" 2>/dev/null | grep -q '__cxx11'; then
        echo "build.sh: existing AWS SDK install was built against libstdc++ — rebuilding with libc++" >&2
        rm -rf ./external/aws-install ./external/aws/build
    fi
fi

if [ -z "$(ls -A ./external/boost-decimal 2>/dev/null)" ] || [ -z "$(ls -A ./external/libpqxx 2>/dev/null)" ] \
    || [ ! -f ./external/aws-install/lib/cmake/AWSSDK/AWSSDKConfig.cmake ]; then
    # bash (not ./) so a lost exec bit can't break the build; abort on failure
    # rather than cascading into a confusing find_package(AWSSDK) error later.
    bash ./scripts/build_dep.sh || { echo "build.sh: dependency build failed" >&2; exit 1; }
fi

BUILD_DIR="build"
# Variables
EXECUTABLE_NAME="BacktestingEngine"

# Step 1: Create a build directory if it doesn't exist
if [ ! -d "$BUILD_DIR" ]; then
    mkdir "$BUILD_DIR"
fi

# Step 2: Navigate to the build directory
cd "$BUILD_DIR" || exit

# Expose paths so CMake finds libpq, and select the Homebrew LLVM toolchain.
# The project uses C++23 modules + `import std;`, which needs the Ninja
# generator and a Clang whose libc++ ships a `std` module (Apple Clang does
# not). COMPILER_PATH points Clang at that libc++ so CMake can locate
# libc++.modules.json.
TOOLCHAIN_ARGS=()
if command -v brew &>/dev/null; then
    export PATH="$(brew --prefix libpq)/bin:$PATH"
    export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export PostgreSQL_ROOT="$(brew --prefix libpq)"

    LLVM_PREFIX="$(brew --prefix llvm)"
    export COMPILER_PATH="$LLVM_PREFIX/lib/c++"
    TOOLCHAIN_ARGS+=(
        -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang"
        -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++"
    )
else
    # Off Homebrew (Linux/CI): use Clang + libc++ so `import std` works, matching
    # the macOS toolchain. CMakeLists adds -stdlib=libc++ and locates the libc++
    # `std` module manifest. Honour $CC/$CXX if the caller pinned a version
    # (e.g. clang-19), otherwise fall back to the unversioned names.
    TOOLCHAIN_ARGS+=(
        -DCMAKE_C_COMPILER="${CC:-clang}"
        -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
    )
fi

# 1. Generate build files. -G Ninja is required for `import std`; -Wno-dev
# silences the (expected) "import std support is experimental" developer note.
# ENABLE_COVERAGE=ON (default OFF) instruments the build for Clang source-based
# coverage — CI sets it on the macOS leg to produce the SonarCloud report.
cmake .. -G Ninja -Wno-dev \
  "${TOOLCHAIN_ARGS[@]}" \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_BUILD_TYPE=Release \
  -DSKIP_BUILD_TEST=ON \
  -DENABLE_COVERAGE="${ENABLE_COVERAGE:-OFF}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Step 4: Compile the project
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake --build . --parallel "$JOBS"

# Step 5: Navigate back to the root directory
cd ..
