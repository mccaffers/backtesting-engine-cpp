cmake_minimum_required(VERSION 3.30)

# CMAKE_OSX_SYSROOT is a macOS-specific setting that specifies the SDK path.
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

project(BacktestingEngine)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure libpqxx build
set(PQXX_LIBRARIES_INSTALL ON)
set(SKIP_BUILD_TEST ON)
set(SKIP_CONFIGURE_LIBPQXX OFF)

# Disable warningsfor external libraries
set(PREV_CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /w")
endif()

# Quiet CMAKE output
set(CMAKE_INSTALL_MESSAGE NEVER)
set(CMAKE_MESSAGE_LOG_LEVEL "WARNING")

# Build libpqxx from source
add_subdirectory(external/libpqxx EXCLUDE_FROM_ALL)

# Collect all .cpp files in the src directory
file(GLOB_RECURSE SOURCES "source/*.cpp")

# main.cpp belongs to the executable only — keeping it in the library too
# means it gets compiled twice and the lib carries a competing main().
list(REMOVE_ITEM SOURCES ${CMAKE_SOURCE_DIR}/source/main.cpp)

# Create a library of your project's code
add_library(BacktestingEngineLib STATIC ${SOURCES})

# Include directories — PUBLIC so they propagate to the executable (and any
# other consumer) through target_link_libraries.
target_include_directories(BacktestingEngineLib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/commands
    ${CMAKE_SOURCE_DIR}/include/utilities
    ${CMAKE_SOURCE_DIR}/include/models
    ${CMAKE_SOURCE_DIR}/include/reporting
    ${CMAKE_SOURCE_DIR}/include/trading
    ${CMAKE_SOURCE_DIR}/include/trading_definitions
    ${CMAKE_SOURCE_DIR}/include/strategies
    ${CMAKE_SOURCE_DIR}/external
)

# Configure OpenMP. On Apple, provide Homebrew libomp hints before discovery.
# brew --prefix resolves the right location on both Apple Silicon
# (/opt/homebrew) and Intel (/usr/local).
if(APPLE)
    execute_process(
        COMMAND brew --prefix libomp
        OUTPUT_VARIABLE LIBOMP_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(OpenMP_C_FLAGS "-Xclang -fopenmp")
    set(OpenMP_CXX_FLAGS "-Xclang -fopenmp")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY ${LIBOMP_PREFIX}/lib/libomp.dylib)
    target_include_directories(BacktestingEngineLib PRIVATE ${LIBOMP_PREFIX}/include)
endif()
find_package(OpenMP REQUIRED)

# Boost (Homebrew) — Boost.Redis is header-only but its implementation is
# compiled into one dedicated TU via <boost/redis/src.hpp> (source/
# boostRedisImpl.cpp), which isolates its macro/SSL pollution from the rest of
# the build. It pulls in Boost.Asio's SSL layer, which needs OpenSSL.
find_package(Boost REQUIRED CONFIG)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(BacktestingEngineLib PUBLIC
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    CURL::libcurl)

add_subdirectory(external/boost-decimal)
target_link_libraries(BacktestingEngineLib PUBLIC Boost::decimal)

target_link_libraries(BacktestingEngineLib PUBLIC pqxx OpenMP::OpenMP_CXX)

# Main executable
add_executable(BacktestingEngine source/main.cpp)
target_link_libraries(BacktestingEngine BacktestingEngineLib)
