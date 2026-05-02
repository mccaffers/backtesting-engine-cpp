PROFILE_DIR=$(ls -t /tmp/Build/ProfileData | head -n 1);
xcrun llvm-cov show \
    /tmp/Build/Products/Debug/tests.xctest/Contents/MacOS/tests \
    -instr-profile="/tmp/Build/ProfileData/${PROFILE_DIR}/Coverage.profdata" \
    -format=text \
    > coverage.txt