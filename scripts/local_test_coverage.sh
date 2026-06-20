#!/bin/bash
# Build the Catch2 unit tests with Clang source-based coverage and emit the
# SonarCloud generic coverage report locally (the same report CI uploads).
# Replaces the old xcodebuild + xccov flow.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

rm -f sonarqube-generic-coverage.xml

# Instrument the build (-DENABLE_COVERAGE=ON via the env passthrough in build.sh).
ENABLE_COVERAGE=ON bash ./scripts/build.sh

# Runs the tests, merges the profile, and converts llvm-cov output to Sonar
# generic XML.
bash ./.github/workflows/scripts/llvmcov-to-sonarqube-generic.sh build \
    > sonarqube-generic-coverage.xml

echo "Wrote sonarqube-generic-coverage.xml"
