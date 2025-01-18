cmake_minimum_required(VERSION 3.30)
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
add_subdirectory(external/libpqxx)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/source/include
    ${CMAKE_SOURCE_DIR}/source/include/strategy
    ${CMAKE_SOURCE_DIR}/external
)

# Collect all .cpp files in the src directory
file(GLOB_RECURSE SOURCES "source/*.cpp")

# Create a library of your project's code
add_library(BacktestingEngineLib STATIC ${SOURCES})

# Link against pqxx
target_link_libraries(BacktestingEngineLib pqxx)

# Main executable
add_executable(BacktestingEngine source/Main.cpp)
target_link_libraries(BacktestingEngine BacktestingEngineLib)
