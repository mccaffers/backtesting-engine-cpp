cmake_minimum_required(VERSION 3.30)
project(BacktestingEngine)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Quiet CMAKE output
set(CMAKE_INSTALL_MESSAGE NEVER)
set(CMAKE_MESSAGE_LOG_LEVEL "WARNING")

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/Models
    ${CMAKE_SOURCE_DIR}/include/Strategies
)

# Source files
set(SOURCES
    src/FileManagement/CSVStream.cpp
    src/Models/PriceRecord.cpp
    src/Strategies/SimpleMovingAverageStrategy.cpp
    src/Strategies/RandomStrategy.cpp
    src/Account.cpp
    src/Application.cpp
    src/CSVParser.cpp
    src/Utilities/Utils.cpp
)

# Create a library of your project's code
add_library(BacktestingEngineLib STATIC ${SOURCES})

# Main executable
add_executable(BacktestingEngine src/Program.cpp)
target_link_libraries(BacktestingEngine BacktestingEngineLib)

# GoogleTest
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.11.0
)
FetchContent_MakeAvailable(googletest)

# Enable testing
enable_testing()

# Test sources
# file(GLOB SOURCES
set(TEST_SOURCES
    tests/PriceRecordTest.cpp
    tests/RandomStrategyTest.cpp
    tests/AccountTest.cpp
)

# Test executable
add_executable(BacktestingEngineTests ${TEST_SOURCES})
target_link_libraries(BacktestingEngineTests BacktestingEngineLib gtest gtest_main)

# Coverage flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")

# Add tests to CTest
include(GoogleTest)
gtest_discover_tests(BacktestingEngineTests)
