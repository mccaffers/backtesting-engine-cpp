// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/execution/runnerBridge.hpp"

import std;
import priceData;       // PriceData
import backtestRunner;  // loadTicks, runBacktestOnTicks

// The tick buffer the opaque handle wraps. Kept out of redisRunner.cpp so that
// TU never has to name PriceData (a module type) or import a module — see
// runnerBridge.hpp for why that matters.
struct LoadedTicks {
    std::vector<PriceData> ticks;
};

std::shared_ptr<const LoadedTicks> bridgeLoadTicks(const std::string& questdbHost,
                                                   const std::string& symbolsCsv,
                                                   int lastMonths) {
    auto loaded = std::make_shared<LoadedTicks>();
    loaded->ticks = loadTicks(questdbHost, symbolsCsv, lastMonths);
    return loaded;
}

void bridgeRunOnTicks(const LoadedTicks& ticks,
                      const tradingDefinitions::Configuration& config) {
    runBacktestOnTicks(ticks.ticks, config);
}
