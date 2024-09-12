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
    ${CMAKE_SOURCE_DIR}/source/include
)

# Collect all .cpp files in the src directory
file(GLOB_RECURSE SOURCES "source/*.cpp")

# Create a library of your project's code
add_library(BacktestingEngineLib STATIC ${SOURCES})

# Main executable
add_executable(BacktestingEngine source/main.cpp)
target_link_libraries(BacktestingEngine BacktestingEngineLib)
