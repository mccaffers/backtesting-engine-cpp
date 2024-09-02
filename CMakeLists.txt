cmake_minimum_required(VERSION 3.10)
project(MyProject)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/Strategies
    ${CMAKE_SOURCE_DIR}/include/FileManagement
)

# Source files
set(SOURCES
    src/PriceRecord.cpp
    src/Account.cpp
    src/CSVParser.cpp
    src/Strategies/SimpleMovingAverageStrategy.cpp
    src/Strategies/RandomStrategy.cpp
    src/FileManagement/CSVStream.cpp
)

# Create a library of your project's code
add_library(MyProjectLib STATIC ${SOURCES})

# Main executable
add_executable(MyProgram src/program.cpp)
target_link_libraries(MyProgram MyProjectLib)

# GoogleTest is Google's C++ testing and mocking framework
# https://github.com/google/googletest
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
set(TEST_SOURCES
    tests/PriceRecordTest.cpp
)

# Test executable
add_executable(MyProgramTests ${TEST_SOURCES})
target_link_libraries(MyProgramTests MyProjectLib gtest gtest_main)

# Add the test
add_test(NAME MyProgramTests COMMAND MyProgramTests)