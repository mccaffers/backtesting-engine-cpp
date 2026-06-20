#!/bin/bash
# Build and run the Catch2 unit tests via CMake/Ninja + ctest. The test suite
# moved off Xcode/XCTest onto Catch2 (see tests/*.cpp); scripts/build.sh selects
# the Clang/libc++ toolchain and builds the unit_tests target as part of `all`.
#
# Pass CLEAN=1 to force a clean reconfigure: CLEAN=1 ./scripts/test.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

if [[ "${CLEAN:-0}" == "1" ]]; then
    rm -rf build
fi

# Configures the toolchain and builds the library, executable, and unit_tests.
bash ./scripts/build.sh

ctest --test-dir build --output-on-failure
