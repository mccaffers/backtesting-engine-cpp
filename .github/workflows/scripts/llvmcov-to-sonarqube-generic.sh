#!/usr/bin/env bash
# Generate a SonarQube "generic test coverage" report from the Catch2 unit tests
# using Clang source-based coverage (replaces the xcodebuild + xccov path).
#
# Pipeline: run the instrumented unit_tests binary -> llvm-profdata merge ->
# llvm-cov export (lcov) -> convert lcov to Sonar generic XML on stdout.
#
# Usage:   bash llvmcov-to-sonarqube-generic.sh [BUILD_DIR] > coverage.xml
#   BUILD_DIR defaults to "build". Progress goes to stderr; only the XML is
#   written to stdout, so the caller can redirect it straight to a file (matching
#   how xccov-to-sonarqube-generic.sh was invoked).
#
# llvm-cov / llvm-profdata are taken from $LLVM_COV / $LLVM_PROFDATA if set,
# else from Homebrew LLVM on macOS, else from PATH (apt.llvm.org on Linux puts
# versioned tools on PATH via update-alternatives).
set -euo pipefail

BUILD_DIR="${1:-build}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TEST_BIN="${BUILD_DIR}/tests/unit_tests"

if [[ ! -x "$TEST_BIN" ]]; then
    echo "Test binary not found at $TEST_BIN — build the unit_tests target with -DENABLE_COVERAGE=ON first" 1>&2
    exit 1
fi

# Resolve the LLVM coverage tools.
if [[ -n "${LLVM_COV:-}" ]]; then
    : # caller-provided
elif [[ "$(uname)" == "Darwin" ]] && command -v brew &>/dev/null; then
    LLVM_BIN="$(brew --prefix llvm)/bin"
    LLVM_COV="${LLVM_BIN}/llvm-cov"
    LLVM_PROFDATA="${LLVM_BIN}/llvm-profdata"
else
    LLVM_COV="llvm-cov"
    LLVM_PROFDATA="llvm-profdata"
fi
LLVM_PROFDATA="${LLVM_PROFDATA:-llvm-profdata}"

PROFRAW="${BUILD_DIR}/coverage/unit_tests.profraw"
PROFDATA="${BUILD_DIR}/coverage/unit_tests.profdata"
mkdir -p "${BUILD_DIR}/coverage"

echo "Running instrumented tests..." 1>&2
LLVM_PROFILE_FILE="$PROFRAW" "$TEST_BIN" 1>&2

echo "Merging profile data..." 1>&2
"$LLVM_PROFDATA" merge -sparse "$PROFRAW" -o "$PROFDATA"

echo "Exporting coverage and converting to Sonar generic XML..." 1>&2
# Only the project's own source is relevant — keep files under source/ and drop
# system headers, Catch2, and the test files themselves.
"$LLVM_COV" export -format=lcov -instr-profile="$PROFDATA" "$TEST_BIN" \
    | awk -v root="${REPO_ROOT}/" '
    function xml_escape(s) {
        gsub(/&/, "\\&amp;", s); gsub(/</, "\\&lt;", s); gsub(/>/, "\\&gt;", s);
        gsub(/"/, "\\&quot;", s); return s;
    }
    BEGIN { print "<coverage version=\"1\">" }
    /^SF:/ {
        path = substr($0, 4);
        sub("^" root, "", path);          # make repo-relative
        keep = (path ~ /^source\//);      # project source only
        if (keep) printf "  <file path=\"%s\">\n", xml_escape(path);
        next;
    }
    /^DA:/ {
        if (!keep) next;
        split(substr($0, 4), a, ",");
        covered = (a[2] + 0 > 0) ? "true" : "false";
        printf "    <lineToCover lineNumber=\"%d\" covered=\"%s\"/>\n", a[1], covered;
        next;
    }
    /^end_of_record/ {
        if (keep) print "  </file>";
        keep = 0;
        next;
    }
    END { print "</coverage>" }'
