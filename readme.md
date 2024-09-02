## C++ Backtesting Engine

### About The Project

This backtesting engine is a personal project to explore financial data with C++

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp)

[Sonarcloud Analysis](https://sonarcloud.io/project/overview?id=mccaffers_backtesting-engine-cpp)

### How to use

Run `sh run.sh`, which will create the build folder using CMake and then run the program.

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