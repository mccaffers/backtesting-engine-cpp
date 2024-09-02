cmake_minimum_required(VERSION 3.10)
project(MyProject)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)

# Include directories
include_directories(include)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/Strategies
)

# Add source files
file(GLOB_RECURSE SOURCES 
    "src/*.cpp"
    "src/Strategies/*.cpp"
    "src/FileManagement/*.cpp"
)

# Add the executable
add_executable(MyProgram ${SOURCES})