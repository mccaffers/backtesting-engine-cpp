# Requirements

System and dependency requirements for building and running the engine. For build/run instructions see [QUICKSTART.md](QUICKSTART.md); for runtime configuration see [ENVIRONMENT.md](ENVIRONMENT.md).

## Toolchain

The project is C++23 with modules and `import std;`, which constrains the toolchain tightly:

| Requirement | Why |
| --- | --- |
| CMake 3.30.x, 4.2.x, or 4.3.x (exactly — not "anything newer") | `import std` is gated behind `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`, whose activation UUID is pinned per CMake version in `CMakeLists.txt` (branches exist for 3.30, 4.2, and 4.3 — other versions fail configure with instructions for adding a branch) |
| Ninja | The only CMake generator that supports C++23 modules / `import std` |
| Clang + libc++ with a `std` module | Apple Clang does **not** ship one. macOS: Homebrew LLVM (`brew install llvm`). Linux: Clang from [apt.llvm.org](https://apt.llvm.org) (CI pins version 20) with `libc++-dev`/`libc++abi-dev` |
| OpenMP runtime | `libomp` (Homebrew) / `libomp-<N>-dev` (apt) — the engine library links OpenMP |

`scripts/build.sh` selects the toolchain automatically: on Homebrew systems it picks Homebrew LLVM and exports `COMPILER_PATH` so Clang finds `libc++.modules.json`; elsewhere it honours `$CC`/`$CXX` (falling back to `clang`/`clang++`).

## System libraries

### macOS (Homebrew)

The canonical CI list lives in `.github/workflows/scripts/brew.sh`:

```
brew install postgresql@18 pkg-config boost llvm ninja libomp
```

`postgresql@18` provides libpq (needed by libpqxx). OpenSSL, CURL, and ZLIB are also required by CMake (`find_package`) and typically already present; `brew install openssl curl` covers them if not.

### Ubuntu / Debian

The canonical CI setup lives in `.github/workflows/scripts/ubuntu_deps.sh`:

```
# Clang + libc++ from apt.llvm.org (LLVM_VERSION defaults to 20)
wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 20

sudo apt-get install libc++-20-dev libc++abi-20-dev libomp-20-dev \
    ninja-build libssl-dev libpq-dev libcurl4-openssl-dev
```

**Boost ≥ 1.84 is required** (Boost.Redis was added in 1.84; Ubuntu's apt Boost predates it). CI builds Boost 1.90.0 from source — only headers plus Boost.System are needed since Boost.Redis and Boost.Asio are header-only. On macOS, `brew install boost` is current enough.

## Vendored dependencies (`external/`)

| Dependency | How it's tracked | How it's built |
| --- | --- | --- |
| [libpqxx](https://github.com/jtv/libpqxx) | git submodule | `add_subdirectory` from the top-level CMakeLists (prebuilt once by `scripts/build_dep.sh`) |
| [boost-decimal](https://github.com/boostorg/decimal) | git submodule | header-only, `add_subdirectory` — nothing to compile |
| [Catch2](https://github.com/catchorg/Catch2) | git submodule | `add_subdirectory`, tests only |
| nlohmann/json | vendored headers | header-only include |
| [aws-sdk-cpp](https://github.com/aws/aws-sdk-cpp) | git submodule (has nested CRT submodules — fetch with `--recursive`) | built + installed once by `scripts/build_dep.sh` into `external/aws-install` (static libs, DynamoDB client only), consumed via `find_package(AWSSDK)` |

Fetch the submodules (the build scripts also do this on first run):

```
git submodule update --init --recursive
```

The AWS SDK is deliberately **not** an `add_subdirectory` — the SDK is enormous and must not inherit the project's module/OpenMP flags. On Linux, `scripts/build_dep.sh` builds it with the same Clang/libc++ toolchain as the engine, and a stale libstdc++-built install is detected (by its `__cxx11` ABI markers) and rebuilt automatically. The DynamoDB client is used by the `live` and `positions` commands to pull IG session credentials (`source/shared/aws/dynamoAuth`).

## Runtime services

None are needed to *build*; which ones you need to *run* depends on the command (see [QUICKSTART.md](QUICKSTART.md)):

| Service | Default endpoint | Needed by |
| --- | --- | --- |
| Redis | `127.0.0.1:6379` | `load`, `run`, `experiments`, `analysis` (work queues); `live` (trade locks, position book, API rate budget); `positions` (position book + cluster-set writes); `tracking` (position-book reads for deal enrichment) |
| QuestDB | pgwire `:8812` (reads), ILP-over-HTTP `:9000` (writes) | `run`, `analysis` (tick reads), `live` (OHLC/range-bar warm-up reads — on by default, `OHLC_PREPOPULATE=0` disables), `ingest` (tick writes) |
| Elasticsearch | `http://localhost:9200` | `run` (results reporting), `experiments`/`analysis` (experiment index admin + aggregate docs), `live` (winner selection + trade audit), `positions` (cycle reports), `tracking` (`live-trades` docs) |
| IG REST API | — | `live` (order placement), `positions` (open-position reads) |
| AWS DynamoDB | `MarketDataLive` table | `live`, `positions` (IG session credentials) |

The `tracking` subcommand binds a UDP socket for the IG streamer's deal feed and logs to stdout; it also enriches each deal from the Redis position book and ships `live-trades` documents to Elasticsearch. Both degrade gracefully (fallback documents, never a crash) when unreachable.

A note on starting QuestDB locally is in [documents/questdb.md](documents/questdb.md).
