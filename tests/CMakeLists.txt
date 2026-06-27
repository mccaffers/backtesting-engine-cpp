# Catch2 unit tests. Each ported file is one translation unit that links the
# project library (BacktestingEngineLib) and consumes its C++23 modules via
# `import`. The library's module FILE_SET and include directories are PUBLIC, so
# they propagate here through the link below.
add_executable(unit_tests
    symbolScale.cpp
    jsonParser.cpp
    db.cpp
    sweep.cpp
    tradeManager.cpp
    tickPacket.cpp
)

target_link_libraries(unit_tests PRIVATE
    BacktestingEngineLib
    Catch2::Catch2WithMain)

# Match the library's module configuration so this consumer can import the
# library's modules (which were built against `import std;`). Without these the
# imported BMIs are rejected as a config mismatch.
set_target_properties(unit_tests PROPERTIES CXX_MODULE_STD ON)
target_compile_features(unit_tests PRIVATE cxx_std_23)

# Register every TEST_CASE with CTest so `ctest` lists/runs them individually.
include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(unit_tests)
