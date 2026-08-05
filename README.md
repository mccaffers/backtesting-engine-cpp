# C++ Backtesting Engine

Active development!

Feel free to explore, but this code base is usable at the moment.

## About The Project

I'm developing a high-performance C++ backtesting engine designed to analyze financial data and evaluate multiple trading strategies at scale — and to take the winners live.

[![Build](https://github.com/mccaffers/backtesting-engine-cpp/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/mccaffers/backtesting-engine-cpp/actions/workflows/build.yml) [![Bugs](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=bugs)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp) [![Code Smells](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=code_smells)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp) [![Coverage](https://sonarcloud.io/api/project_badges/measure?project=mccaffers_backtesting-engine-cpp&metric=coverage)](https://sonarcloud.io/summary/new_code?id=mccaffers_backtesting-engine-cpp)

The engine is C++23 (modules, `import std;`) and one binary with eight subcommands:

- **`ingest`** — receives a UDP tick stream and writes it to QuestDB
- **`load`** — expands a strategy parameter sweep and queues it in Redis
- **`run`** — drains the queue, backtests against QuestDB ticks, reports results to Elasticsearch
- **`experiments`** — expands a parameter sweep of occurrence-rate questions ("price drops 1% in 10m, then recovers 0.5% in 10m — how often?") and queues it in Redis, no strategy required
- **`analysis`** — drains the experiment queue, counts pattern occurrences against QuestDB ticks, reports aggregate stats (rates, conditional completion, excursion quantiles) to Elasticsearch
- **`live`** — takes the winning backtests from Elasticsearch and trades them live via the IG REST API
- **`tracking`** — receives the IG account's deal/position updates over UDP, logs each one, archives closed deals in the Redis position book (`PO#` → `PH#`, pruning the `PL#` list), and ships a `live-trades` document to Elasticsearch per deal
- **`positions`** — mirrors the IG account's open positions into Redis every minute (the position book, strategy lists, and cluster-exposure sets the live engine reads)

I'm extracting results and creating various graphs for trend analyses using SciPy for calculations and Plotly for visualization.

![alt text](documents/images/random-indices-sp500-variable.svg)

*Read more results on https://mccaffers.com/quantitative_analysis/randomly_trading/*

## Documentation

| Document | Contents |
| --- | --- |
| [QUICKSTART.md](QUICKSTART.md) | Building the engine and using each subcommand |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How it fits together — data flow, queue design, live order path (Mermaid diagrams) |
| [ENVIRONMENT.md](ENVIRONMENT.md) | Every environment variable, per command, with defaults |
| [REQUIREMENTS.md](REQUIREMENTS.md) | Toolchain, system libraries, vendored dependencies, runtime services |

## Quick start

```bash
git clone --recurse-submodules https://github.com/mccaffers/backtesting-engine-cpp
cd backtesting-engine-cpp

bash ./scripts/build.sh    # CMake + Ninja + Clang/libc++ (see REQUIREMENTS.md for the toolchain)
bash ./scripts/test.sh     # Catch2 tests via ctest

# with Redis, QuestDB, and Elasticsearch running (see QUICKSTART.md):
./build/BacktestingEngine load random    # queue a sweep
./build/BacktestingEngine run localhost  # drain and backtest
```

## Contributing

This is an active solo experiment, so I'm not accepting pull requests right now, but please fork freely and use [GitHub Issues](https://github.com/mccaffers/backtesting-engine-cpp/issues) for bugs, questions, and ideas. See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

## License

[MIT](https://choosealicense.com/licenses/mit/)
