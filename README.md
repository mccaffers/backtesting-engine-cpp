## C++ Backtesting Engine

Active development!

Feel free to explore, but this code base is usuable at the moment.

### About The Project

I'm developing a high-performance C++ backtesting engine designed to analyze financial data and evaluate multiple trading strategies at scale.

[![Build](https://github.com/mccaffers/backtesting-engine-cpp/actions/workflows/sonarcloud.yml/badge.svg)](https://github.com/mccaffers/backtesting-engine-cpp/actions/workflows/sonarcloud.yml)  [![Bugs](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=bugs)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp) [![Code Smells](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=code_smells)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp) [![Coverage](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=coverage)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp)

I'm extracting results and creating various graphs for trend analyses using SciPy for calculations and Plotly for visualization.

![alt text](images/random-indices-sp500-variable.svg)

*Read more results on https://mccaffers.com/randomly_trading/*

## Setup

This backtesting engine can pull tick data from local files or from a Postgres database. I'm using QuestDB.

### Postgres Setup - Requires libpq-dev or its equivalent for your OS:

```
For Ubuntu/Debian systems: sudo apt-get install libpq-dev
On Red Hat Linux (RHEL) systems: yum install postgresql-devel
For Mac Homebrew: brew install postgresql
For OpenSuse: zypper in postgresql-devel
For ArchLinux: pacman -S postgresql-libs
```

### Postgres Setup (using C++20)

```
cd ./external/libpqxx
mkdir -p build
cd ./build
cmake ..
./configure CXXFLAGS="-std=c++20 -O3"
make
```

Xcode - Link Binary with Libraries (Source & Test)

```
./build/external/libpqxx/src/libpqxx-7.10.a
```

Xcode - Headers Path (for libpqxx and nlohmann/json)

``` 
"$(SRCROOT)/external/libpqxx/include/pqxx/internal"
"$(SRCROOT)/external/libpqxx/include/"
"$(SRCROOT)/external/"
```

Xcode - Library Path

```
"$(SRCROOT)/external/libpqxx/src"
"$(SRCROOT)/build/external/libpqxx/src"
"/opt/homebrew/Cellar/postgresql@14/14.15/lib/postgresql@14"
```

### Test the build

`sh ./scripts/build.sh`

### Run via terminal

`sh ./scripts/run.sh`

### Run via test via terminal

`sh ./scripts/test.sh`

### License
[MIT](https://choosealicense.com/licenses/mit/)
