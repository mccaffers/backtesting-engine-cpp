#!/bin/bash

if [[ "$(uname)" != "Darwin" ]]; then
    echo "Testing is done via Xcode build — C++ methods are wrapped in Objective-C for inline debugging and testing in Xcode."
    exit 0
fi

# Pass CLEAN=1 to force a clean build: CLEAN=1 ./scripts/test.sh
CLEAN_ACTION=""
if [[ "${CLEAN:-0}" == "1" ]]; then
    CLEAN_ACTION="clean"
fi

xcodebuild \
    -project backtesting-engine-cpp.xcodeproj \
    -scheme tests \
    -parallelizeTargets \
    -jobs "$(sysctl -n hw.logicalcpu)" \
    CODE_SIGN_IDENTITY="-" \
    ENABLE_TESTABILITY=YES \
    ${CLEAN_ACTION} build test 2>&1 | xcpretty