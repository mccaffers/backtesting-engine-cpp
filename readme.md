## C++ Backtesting Engine

### About The Project

This backtesting engine is a personal project to explore financial data with C++

[Sonarcloud Analysis](https://sonarcloud.io/project/overview?id=mccaffers_backtesting-engine-cpp)

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