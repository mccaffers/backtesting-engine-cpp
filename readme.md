## C++ Backtesting Engine

> [!NOTE]  
> Work in progress, Jan 2025

### About The Project

This backtesting engine is a personal project to explore financial data with C++

[![Build](https://github.com/mccaffers/backtesting-engine-cpp/actions/workflows/sonarcloud.yml/badge.svg)](https://github.com/mccaffers/backtesting-engine-cpp/actions/workflows/sonarcloud.yml)  [![Bugs](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=bugs)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp) [![Code Smells](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=code_smells)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp) [![Coverage](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=coverage)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp)

I'm extracting results and creating various graphs for trend analyses using SciPy for calculations and Plotly for visualization.

![alt text](images/random-indices-sp500-variable.svg)

*Read more results on https://mccaffers.com/randomly_trading/*


## Setup

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
````
./build/external/libpqxx/src/libpqxx-7.10.a
```

Xcode - Headers Path
``` 
"$(SRCROOT)/external/libpqxx/include/pqxx/internal"
"$(SRCROOT)/external/libpqxx/include/"
```

Xcode - Library Path
```
"$(SRCROOT)/external/libpqxx/src"
```

### Test build with cmake

`sh ./scripts/build.sh`

### Run via terminal

`sh ./scripts/run.sh`

### Run via test via terminal

`sh ./scripts/test.sh`

### Project Structure

```bash
project/
.
├── CMakeLists.txt
├── include
│   ├── Account.h
│   ├── CSVParser.h
│   ├── PriceRecord.h
│   ├── Strategies
│   │   └── SimpleMovingAverageStrategy.h
│   └── Strategy.h
├── readme.md
├── resources
├── run.sh
└── src
    ├── Account.cpp
    ├── FileManagement
    │   ├── CSVParser.cpp
    │   └── PriceRecord.cpp
    ├── Strategies
    │   └── SimpleMovingAverageStrategy.cpp
    └── program.cpp
```

### License
[MIT](https://choosealicense.com/licenses/mit/)
