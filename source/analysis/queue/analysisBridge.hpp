// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <cstddef>
#include <memory>
#include <string>

#include "shared/experiments/experimentConfig.hpp"
#include "analysis/reporting/experimentResults.hpp"

// The analysis worker's import boundary to the backtestRunner / chainMatcher /
// experimentElastic modules — runnerBridge.hpp's exact doctrine:
// drainExperiments.cpp must stay a *purely textual* TU because its ThreadPool
// instantiates std::condition_variable_any::wait(lock, stop_token, pred),
// which the toolchain miscompiles when the same TU also imports a module. So
// the drain loop reaches the modules only through these plain (global-module)
// functions, never naming PriceData or importing anything itself.

// Opaque handle to one run's tick buffer, defined in analysisBridge.cpp.
struct ExperimentTicksImpl;

// Value-copyable without naming any module type: pool tasks capture it BY
// VALUE, so the shared_ptr keeps the tick buffer alive for the lifetime of
// each evaluation even after the drain loop moves to the next run. No
// TickCache in v1 — each experiment run is its own one-window load, so a
// cache would be all misses; the shared_ptr alone gives buffer-outlives-pool
// safety.
struct ExperimentTicks {
    std::shared_ptr<const ExperimentTicksImpl> impl;
    std::size_t tickCount = 0;  // logging only
};

// One QuestDB load for the run's symbols/window (backtestRunner's loadTicks).
ExperimentTicks bridgeLoadExperimentTicks(const std::string& questdbHost,
                                          const std::string& symbolsCsv,
                                          int lastMonths,
                                          int offsetMonths);

// Replays one experiment over the loaded ticks (chain_matcher's
// evaluateExperiment — throws std::invalid_argument on a malformed chain).
experiments::ExperimentOutcome bridgeEvaluateExperiment(
    const ExperimentTicks& ticks,
    const experiments::ExperimentConfig& config);

// Queues the aggregate document for Elasticsearch
// (ExperimentElastic::putExperimentResults).
void bridgePutExperimentResults(const ExperimentResults& results);
