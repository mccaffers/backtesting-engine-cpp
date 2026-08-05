#!/bin/bash
# Fail fast: -e exits on any error, -u errors on undefined vars,
# pipefail propagates failures through pipes (e.g. `foo | grep`).
set -euo pipefail

git submodule update --init --recursive

# Resolve paths relative to this script, not the caller's working directory,
# so the script works no matter where it's invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTERNAL_DIR="$SCRIPT_DIR/../external"

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# On macOS, pin the SDK the same way the top-level CMakeLists does, via
# `xcrun --show-sdk-path`. Without this, the standalone dependency build picks
# whatever SDK CMake guesses and caches it — which breaks the moment that SDK
# path changes (e.g. an Xcode update), leaving clang pointed at a deleted
# sysroot and unable to find the C++ standard headers. Empty (and unused) off
# macOS, where xcrun does not exist.
SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"

# Build a CMake dependency. The body runs in a subshell `( ... )`,
# so the `cd` only affects this build — control returns to the caller
# in its original directory automatically.
build_dep() {
    local dep_dir="$1"
    local -a cmake_args=(
        -DCMAKE_CXX_STANDARD=23
        -DCMAKE_BUILD_TYPE=Release
        -DSKIP_BUILD_TEST=ON
        -DCMAKE_CXX_FLAGS="-w"
        -DCMAKE_C_FLAGS="-w"
    )
    [ -n "$SDK_PATH" ] && cmake_args+=("-DCMAKE_OSX_SYSROOT=$SDK_PATH")
    (
        cd "$dep_dir"
        # Drop any stale CMake cache so a previously recorded (and possibly
        # now-removed) SDK/compiler path can't poison the reconfigure.
        rm -rf build
        mkdir -p build
        cd build
        cmake .. "${cmake_args[@]}"
        cmake --build . --parallel "$JOBS"
    )
}

# libpqxx needs libpq's headers/pkg-config; expose the Homebrew install.
# Setting these once at the top is fine — boost-decimal ignores them.
if command -v brew &>/dev/null; then
    export PATH="$(brew --prefix libpq)/bin:$PATH"
    export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export PostgreSQL_ROOT="$(brew --prefix libpq)"
fi

# boost-decimal is header-only (an INTERFACE CMake target) — there's nothing
# to compile. The parent CMakeLists pulls it in via add_subdirectory(),
# which is all a header-only lib needs. We just need the submodule fetched
# above; no build step.
build_dep "$EXTERNAL_DIR/libpqxx"

# AWS SDK for C++ — DynamoDB client only (IG session credentials, see
# shared/aws/dynamoAuth). Unlike libpqxx it is NOT add_subdirectory'd into
# the main build (it is enormous, and the main build's module/OpenMP flags
# must not leak into it): it is built and INSTALLED once here, then found by
# the main CMakeLists via find_package(AWSSDK) against external/aws-install.
# Static libs so the engine binary carries no SDK dylibs. The source is the
# external/aws submodule, populated (with its nested CRT submodules) by the
# `git submodule update --init --recursive` at the top of this script.
#
# Because these static libs are linked into the engine, they MUST be built
# against the same C++ standard library as the main build. On Linux the
# engine is Clang + libc++ (required for `import std`), but CMake's default
# compiler pick is GCC/libstdc++ — that combination links the SDK's
# libstdc++-ABI symbols (std::__cxx11::…) into the .a files and they never
# resolve at the engine's libc++ link. Mirror build.sh's toolchain: honour
# $CC/$CXX, fall back to clang, and force -stdlib=libc++ off macOS (macOS
# clang already defaults to libc++, and the Homebrew/Apple split there is
# ABI-compatible, so leave that path untouched).
AWS_CMAKE_ARGS=()
AWS_CXX_FLAGS="-w"
if ! command -v brew &>/dev/null; then
    AWS_CMAKE_ARGS+=(
        -DCMAKE_C_COMPILER="${CC:-clang}"
        -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
    )
    AWS_CXX_FLAGS="-w -stdlib=libc++"
fi

AWS_SRC="$EXTERNAL_DIR/aws"
AWS_INSTALL="$EXTERNAL_DIR/aws-install"

# Auto-heal installs produced by the old default-toolchain build: on Linux
# those are GCC/libstdc++ artifacts, recognisable by the __cxx11 ABI marker
# in their symbols. Nuke and rebuild rather than skipping below.
if ! command -v brew &>/dev/null; then
    # `|| true`: on a fresh node aws-install doesn't exist yet, and a failing
    # `find` under `set -e -o pipefail` would silently kill the whole script.
    AWS_CORE_LIB="$(find "$AWS_INSTALL" -name 'libaws-cpp-sdk-core.a' 2>/dev/null | head -n1 || true)"
    if [ -n "$AWS_CORE_LIB" ] && nm "$AWS_CORE_LIB" 2>/dev/null | grep -q '__cxx11'; then
        echo "build_dep: existing AWS SDK install was built against libstdc++ — rebuilding with libc++" >&2
        rm -rf "$AWS_INSTALL" "$AWS_SRC/build"
    fi
fi

if [ ! -f "$AWS_SRC/CMakeLists.txt" ]; then
    echo "build_dep: external/aws submodule not populated — run 'git submodule update --init --recursive'" >&2
    exit 1
elif [ -f "$AWS_INSTALL/lib/cmake/AWSSDK/AWSSDKConfig.cmake" ]; then
    echo "build_dep: AWS SDK already installed at external/aws-install — skipping"
else
    # ${arr[@]+...} guards the empty-array expansion, which errors under
    # `set -u` on the bash 3.2 that macOS ships.
    cmake -S "$AWS_SRC" -B "$AWS_SRC/build" -G Ninja \
        ${AWS_CMAKE_ARGS[@]+"${AWS_CMAKE_ARGS[@]}"} \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_ONLY="dynamodb" \
        -DBUILD_SHARED_LIBS=OFF \
        -DFORCE_SHARED_CRT=OFF \
        -DENABLE_TESTING=OFF \
        -DAUTORUN_UNIT_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX="$AWS_INSTALL" \
        -DCMAKE_CXX_FLAGS="$AWS_CXX_FLAGS" \
        -DCMAKE_C_FLAGS="-w" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        ${SDK_PATH:+-DCMAKE_OSX_SYSROOT="$SDK_PATH"}
    cmake --build "$AWS_SRC/build" --parallel "$JOBS"
    cmake --install "$AWS_SRC/build"
fi
