# #!/bin/bash

xcodebuild \
    -project backtesting-engine-cpp.xcodeproj \
    -scheme tests \
    clean build test
    # HEADER_SEARCH_PATHS="./external/libpqxx/include/pqxx/internal ./external/libpqxx/include/ ./external/libpqxx/build/include/ ./external/" \
    # LIBRARY_SEARCH_PATHS="./external/libpqxx/src/ ./external/libpqxx/build/src/" \
    # OTHER_LDFLAGS="-L./external/libpqxx/build/src -lpqxx -lpq -L$(brew --prefix pkgconf)/lib -L$(brew --prefix pkgconf)/lib/pkgconfig -L$(brew --prefix postgresql@18)/lib/postgresql -L$(brew --prefix postgresql@18)/lib/postgresql/pgxs -L$(brew --prefix postgresql@18)/lib/postgresql/pkgconfig" \


    