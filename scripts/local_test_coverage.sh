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
HEADER_SEARCH_PATHS="./external/libpqxx/include/pqxx/internal ./external/libpqxx/include/ ./external/libpqxx/build/include/ ./external/" \
LIBRARY_SEARCH_PATHS="./external/libpqxx/src/ ./external/libpqxx/build/src/" \
OTHER_LDFLAGS="-L./external/libpqxx/build/src -lpqxx -lpq -L/opt/homebrew/Cellar/pkgconf/2.3.0_1/lib -L/opt/homebrew/Cellar/pkgconf/2.3.0_1/lib/pkgconfig -L/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14 -L/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14/pgxs -L/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14/pkgconfig" \
clean build test

 bash ./.github/workflows/xccov-to-sonarqube-generic.sh *.xcresult/ > sonarqube-generic-coverage.xml