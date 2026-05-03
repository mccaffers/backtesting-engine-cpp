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

# Build a CMake dependency. The body runs in a subshell `( ... )`,
# so the `cd` only affects this build — control returns to the caller
# in its original directory automatically.
build_dep() {
    local dep_dir="$1"
    (
        cd "$dep_dir"
        mkdir -p build
        cd build
        cmake .. \
          -DCMAKE_CXX_STANDARD=20 \
          -DCMAKE_BUILD_TYPE=Release \
          -DSKIP_BUILD_TEST=ON
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
