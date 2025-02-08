# #!/bin/bash

xcodebuild \
    -project backtesting-engine-cpp.xcodeproj \
    -scheme tests \
    HEADER_SEARCH_PATHS="./external/libpqxx/include/pqxx/internal ./external/libpqxx/include/ ./external/libpqxx/build/include/ ./external/" \
    LIBRARY_SEARCH_PATHS="./external/libpqxx/src/ ./external/libpqxx/build/src/" \
    OTHER_LDFLAGS="-L./external/libpqxx/build/src -lpqxx -lpq -L/opt/homebrew/Cellar/pkgconf/2.3.0_1/lib -L/opt/homebrew/Cellar/pkgconf/2.3.0_1/lib/pkgconfig -L/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14 -L/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14/pgxs -L/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14/pkgconfig" \
    clean build test