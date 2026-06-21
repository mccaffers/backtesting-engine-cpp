// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <memory>
#include <string>
#include "shared/tradingDefinitions/config/configuration.hpp"

// Opaque handle to a run's loaded tick data, defined in runnerBridge.cpp.
//
// runnerBridge.cpp is the import boundary to the backtestRunner module.
// redisRunner.cpp must stay a *purely textual* TU: its ThreadPool instantiates
// std::condition_variable_any::wait(lock, stop_token, pred), whose libc++
// internal helper (__atomic_unique_lock::__set_locked_bit) the toolchain fails
// to emit when the same TU also imports a module. So redisRunner.cpp reaches
// the module only through these plain (global-module) functions, never naming
// PriceData or importing anything itself.
struct LoadedTicks;

// Pulls a run's ticks out of QuestDB once; the handle is shared so tasks can
// hold it for the lifetime of their backtest.
std::shared_ptr<const LoadedTicks> bridgeLoadTicks(const std::string& questdbHost,
                                                   const std::string& symbolsCsv,
                                                   int lastMonths);

// Runs a single backtest against already-loaded ticks (no QuestDB access).
void bridgeRunOnTicks(const LoadedTicks& ticks,
                      const tradingDefinitions::Configuration& config);
