cmake_minimum_required(VERSION 3.30)

# CMAKE_OSX_SYSROOT is a macOS-specific setting that specifies the SDK path.
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# Enable `import std;` in C++23 modules. This is gated behind CMake's
# experimental flag whose activation UUID is pinned to the exact CMake version,
# so the value differs between the terminal's CMake and CLion's bundled CMake.
# Select it by version to keep both environments configuring cleanly. MUST be set
# before project()/CXX language enablement so the toolchain is detected with
# import-std support. Requires the Ninja generator plus a toolchain that ships a
# libc++ `std` module. Configure with: cmake -G Ninja -B build
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3" AND CMAKE_VERSION VERSION_LESS "4.4")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "4.2" AND CMAKE_VERSION VERSION_LESS "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30" AND CMAKE_VERSION VERSION_LESS "3.31")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
else()
    message(FATAL_ERROR
        "No pinned CMAKE_EXPERIMENTAL_CXX_IMPORT_STD UUID for CMake "
        "${CMAKE_VERSION}. Find the value next to the "
        "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD symbol in this CMake's binary and add "
        "a branch above.")
endif()

# CMake 4.2 locates the libc++ `std` module via a `libc++.modules.json` manifest,
# discovered through `clang++ -print-file-name=libc++.modules.json`. Homebrew LLVM
# ships the manifest under <prefix>/lib/c++/, which is not a -print-file-name search
# path, so discovery returns the bare filename and import-std detection fails with
# "`libc++.modules.json` resource does not exist". Point CMake straight at the
# manifest. (CMake 3.30 didn't require this, so the terminal build was unaffected.)
# Must be set before project()/CXX enablement, like the experimental flag above.
if(APPLE AND NOT CMAKE_CXX_STDLIB_MODULES_JSON)
    execute_process(
        COMMAND brew --prefix llvm
        OUTPUT_VARIABLE _llvm_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_llvm_prefix AND EXISTS "${_llvm_prefix}/lib/c++/libc++.modules.json")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${_llvm_prefix}/lib/c++/libc++.modules.json")
    endif()
endif()

# On Linux the import-std toolchain is Clang + libc++ (apt.llvm.org), mirroring
# the macOS Homebrew-LLVM setup so both platforms share one story. Stock Clang on
# Linux defaults to libstdc++, which has no usable `std` module, so force libc++
# and locate its module manifest the same way the Apple branch does — Clang's
# `-print-file-name` search path does not include libc++'s install dir, so fall
# back to the well-known apt.llvm.org locations. Gated on the compiler name
# (passed via -DCMAKE_CXX_COMPILER before project()) since the compiler id is not
# known until project()/CXX enablement below. MUST run before project(), like the
# Apple block, so the toolchain is detected with libc++/import-std support.
if(NOT APPLE AND CMAKE_CXX_COMPILER MATCHES "clang")
    string(APPEND CMAKE_CXX_FLAGS " -stdlib=libc++")
    if(NOT CMAKE_CXX_STDLIB_MODULES_JSON)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -stdlib=libc++ -print-file-name=libc++.modules.json
            OUTPUT_VARIABLE _libcxx_modules_json
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT (_libcxx_modules_json AND EXISTS "${_libcxx_modules_json}"))
            file(GLOB _libcxx_modules_json
                /usr/lib/llvm-*/lib/c++/libc++.modules.json
                /usr/lib/*/c++/libc++.modules.json
                /usr/local/lib/c++/libc++.modules.json)
            if(_libcxx_modules_json)
                list(GET _libcxx_modules_json 0 _libcxx_modules_json)  # first match
            endif()
        endif()
        if(_libcxx_modules_json AND EXISTS "${_libcxx_modules_json}")
            set(CMAKE_CXX_STDLIB_MODULES_JSON "${_libcxx_modules_json}")
        endif()
    endif()
endif()

project(BacktestingEngine)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emit build/compile_commands.json for clangd/IntelliSense. Forced on here (not
# left to a configure-time flag) so the project's own source/ entries — and the
# module flags for loadCommand.cppm — are always present for the editor.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Homebrew's libpq is keg-only, so CMake's FindPostgreSQL module can't locate it
# on its own. Without a hint, libpqxx's config.cmake falls back to pkg-config,
# which sets PostgreSQL_FOUND but never creates the PostgreSQL::PostgreSQL imported
# target that src/CMakeLists.txt links against — producing a "target not found"
# generate error. Point FindPostgreSQL at the keg and resolve the target here so it
# exists before libpqxx is configured.
if(APPLE AND NOT PostgreSQL_ROOT)
    execute_process(
        COMMAND brew --prefix libpq
        OUTPUT_VARIABLE PostgreSQL_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()
find_package(PostgreSQL REQUIRED)

# Build libpqxx from source
add_subdirectory(external/libpqxx EXCLUDE_FROM_ALL)

# Collect all .cpp files in the src directory. CONFIGURE_DEPENDS re-runs the
# glob when .cpp files are added/removed (matching the MODULE_INTERFACES glob
# below), so a new translation unit is picked up without a manual reconfigure.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "source/*.cpp")

# main.cpp belongs to the executable only — keeping it in the library too
# means it gets compiled twice and the lib carries a competing main().
list(REMOVE_ITEM SOURCES ${CMAKE_SOURCE_DIR}/source/main.cpp)

# Create a library of your project's code
add_library(BacktestingEngineLib STATIC ${SOURCES})

# C++23 module interface units. The SOURCES glob only matches *.cpp, so the
# .cppm modules are collected separately. CONFIGURE_DEPENDS re-runs the glob when
# files are added/removed. PUBLIC so linked consumers can `import` them.
file(GLOB_RECURSE MODULE_INTERFACES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/source/*.cppm")
target_sources(BacktestingEngineLib
    PUBLIC FILE_SET CXX_MODULES FILES
    ${MODULE_INTERFACES})

# Enable `import std;` for this target only (NOT globally — that would force it
# onto the third-party libpqxx/pqxx targets). Request C++23 as a compile feature
# so import-std support propagates to consumers that link this library.
set_target_properties(BacktestingEngineLib PROPERTIES CXX_MODULE_STD ON)
target_compile_features(BacktestingEngineLib PUBLIC cxx_std_23)

# Include directories — PUBLIC so they propagate to the executable (and any
# other consumer) through target_link_libraries. Every project #include is
# source-relative and path-qualified (e.g. "shared/utilities/decimalJson.hpp",
# "run/reporting/tradingResults.hpp"), so a single source root makes each
# #include / module GMF dependency self-describing. The external root is kept
# only for the vendored header-only nlohmann/json (no CMake target ships it).
target_include_directories(BacktestingEngineLib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
    ${CMAKE_SOURCE_DIR}/external   # vendored header-only nlohmann/json
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

# OpenMP is linked PUBLIC below, so `-Xclang -fopenmp` is applied to every TU in
# the library — including the `.cppm` modules. CMake builds the `import std;`
# standard-library module WITHOUT OpenMP, so importing std from an OpenMP-enabled
# module is rejected by Clang as a BMI config mismatch. That CMake-managed std
# target ignores target_compile_options and linked usage requirements — it honors
# only the global CMAKE_CXX_FLAGS. So put the OpenMP flag there too, making the
# std module's OpenMP configuration match every consumer's. (It then appears twice
# on normal TUs, via this flag and the linked target; a harmless duplicate.)
string(APPEND CMAKE_CXX_FLAGS " ${OpenMP_CXX_FLAGS}")

# Boost (Homebrew) — Boost.Redis is header-only but its implementation is
# compiled into one dedicated TU via <boost/redis/src.hpp>
# (source/shared/redis/connection/boostRedisImpl.cpp), which isolates its
# macro/SSL pollution from the rest of the build. It pulls in Boost.Asio's SSL
# layer, which needs OpenSSL.
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

# Clang source-based coverage. Opt-in (CI sets -DENABLE_COVERAGE=ON) so local
# builds aren't instrumented. PUBLIC on the library so the flags also reach the
# test executable that links it — that's the TU set we actually measure.
option(ENABLE_COVERAGE "Instrument with Clang source-based coverage" OFF)
if(ENABLE_COVERAGE)
    target_compile_options(BacktestingEngineLib PUBLIC -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(BacktestingEngineLib PUBLIC -fprofile-instr-generate -fcoverage-mapping)
endif()

# Unit tests (Catch2). include(CTest) defines BUILD_TESTING (default ON) and
# enables `ctest`. Catch2 is a vendored submodule; EXCLUDE_FROM_ALL keeps it out
# of the default `all` build until the test target pulls it in.
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(external/Catch2 EXCLUDE_FROM_ALL)
    add_subdirectory(tests)
endif()
