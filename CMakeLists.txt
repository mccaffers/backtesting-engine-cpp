cmake_minimum_required(VERSION 3.10)
project(MyProject)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)

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
    src/CSVParser.cpp
)

# Create a library of your project's code
add_library(MyProjectLib STATIC ${SOURCES})

# Main executable
add_executable(MyProgram src/program.cpp)
target_link_libraries(MyProgram MyProjectLib)

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
add_executable(MyProgramTests ${TEST_SOURCES})
target_link_libraries(MyProgramTests MyProjectLib gtest gtest_main)

# Coverage flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")

# Add tests to CTest
include(GoogleTest)
gtest_discover_tests(MyProgramTests)

# Coverage target
# if(CMAKE_COMPILER_IS_GNUCXX)
#     include(CodeCoverage)
#     setup_target_for_coverage_lcov(
#         NAME coverage
#         EXECUTABLE ctest -j ${PROCESSOR_COUNT}
#         DEPENDENCIES MyProgramTests
#     )
# endif()
