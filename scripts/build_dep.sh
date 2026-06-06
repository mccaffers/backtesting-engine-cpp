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
        -DCMAKE_CXX_STANDARD=20
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
