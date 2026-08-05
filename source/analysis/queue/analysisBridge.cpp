// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "analysis/queue/analysisBridge.hpp"

import std;
import priceData;         // PriceData
import backtestRunner;    // loadTicks
import chainMatcher;      // chain_matcher::evaluateExperiment
import experimentElastic; // ExperimentElastic::putExperimentResults

// The tick buffer the opaque handle wraps. Kept out of drainExperiments.cpp
// so that TU never has to name a module type — see analysisBridge.hpp.
struct ExperimentTicksImpl {
    std::vector<PriceData> ticks;
};

ExperimentTicks bridgeLoadExperimentTicks(const std::string& questdbHost,
                                          const std::string& symbolsCsv,
                                          const int lastMonths,
                                          const int offsetMonths) {
    auto impl = std::make_shared<ExperimentTicksImpl>(ExperimentTicksImpl{
        loadTicks(questdbHost, symbolsCsv, lastMonths, offsetMonths)});
    const std::size_t count = impl->ticks.size();
    return ExperimentTicks{std::move(impl), count};
}

experiments::ExperimentOutcome bridgeEvaluateExperiment(
    const ExperimentTicks& ticks,
    const experiments::ExperimentConfig& config) {
    return chain_matcher::evaluateExperiment(ticks.impl->ticks, config);
}

void bridgePutExperimentResults(const ExperimentResults& results) {
    ExperimentElastic::putExperimentResults(results);
}
