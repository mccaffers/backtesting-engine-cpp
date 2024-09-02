cmake_minimum_required(VERSION 3.10)
project(MyProject)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)

# Include directories
include_directories(include)

# Add source files
file(GLOB SOURCES "src/*.cpp" "src/FileManagement/*.cpp")

# Add the executable
add_executable(MyProgram ${SOURCES})