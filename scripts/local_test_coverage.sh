#!/bin/bash
rm -rf ./TestResult;
rm -rf ./TestResult.xcresult;
rm -rf ./sonarqube-generic-coverage.xml

OTHER_CFLAGS="-fprofile-instr-generate -fcoverage-mapping" \
OTHER_CPLUSPLUSFLAGS="-fprofile-instr-generate -fcoverage-mapping" \
OTHER_SWIFT_FLAGS="-profile-generate -profile-coverage-mapping" \
LLVM_PROFILE_FILE="/tmp/coverage.profraw" \
CODE_SIGN_IDENTITY="" CODE_SIGNING_REQUIRED=NO \
xcodebuild \
-scheme tests \
-destination 'platform=macOS' \
-resultBundlePath TestResult/ \
-enableCodeCoverage YES \
-derivedDataPath "/tmp" \
clean build test

bash ./.github/workflows/scripts/xccov-to-sonarqube-generic.sh *.xcresult/ > sonarqube-generic-coverage.xml