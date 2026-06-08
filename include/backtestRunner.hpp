// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <string>
#include <vector>
#include "models/priceData.hpp"
#include "trading_definitions/configuration.hpp"

// Pulls all tick data for the run's symbols/window out of QuestDB. Expensive —
// call once per run and reuse the result across that run's strategies.
std::vector<PriceData> loadTicks(const std::string& questdbHost,
                                 const std::string& symbolsCsv,
                                 int lastMonths);

// Runs one backtest against already-loaded ticks (no QuestDB access).
void runBacktestOnTicks(const std::vector<PriceData>& ticks,
                        const trading_definitions::Configuration& config);

// Convenience for the direct path: loads ticks then runs a single backtest.
int runBacktest(const std::string& questdbHost,
                const trading_definitions::Configuration& config);
