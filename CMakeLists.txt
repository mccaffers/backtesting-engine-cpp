cmake_minimum_required(VERSION 3.30)

# CMAKE_OSX_SYSROOT is a macOS-specific setting that specifies the SDK path.
# This is ignored on non-Apple platforms, so it's safe to include in cross-platform builds.
execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

project(BacktestingEngine)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)
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

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/utilities
    ${CMAKE_SOURCE_DIR}/include/models
    ${CMAKE_SOURCE_DIR}/include/trading
    ${CMAKE_SOURCE_DIR}/include/trading_definitions
    ${CMAKE_SOURCE_DIR}/external
)

# Collect all .cpp files in the src directory
file(GLOB_RECURSE SOURCES "source/*.cpp")

# Create a library of your project's code
add_library(BacktestingEngineLib STATIC ${SOURCES})

# Configure OpenMP. On Apple, provide Homebrew libomp hints before discovery.
if(APPLE)
    set(OpenMP_C_FLAGS "-Xclang -fopenmp")
    set(OpenMP_CXX_FLAGS "-Xclang -fopenmp")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY /opt/homebrew/opt/libomp/lib/libomp.dylib)
    target_include_directories(BacktestingEngineLib PRIVATE /opt/homebrew/opt/libomp/include)
endif()

find_package(OpenMP REQUIRED)
target_link_libraries(BacktestingEngineLib PUBLIC pqxx OpenMP::OpenMP_CXX)

# Main executable
add_executable(BacktestingEngine source/main.cpp)
target_link_libraries(BacktestingEngine BacktestingEngineLib)
