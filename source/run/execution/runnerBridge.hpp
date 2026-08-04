// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include "shared/tradingDefinitions/config/configuration.hpp"

// Opaque handle to a run's tick window, defined in runnerBridge.cpp.
//
// runnerBridge.cpp is the import boundary to the backtestRunner/tickCache
// modules. redisRunner.cpp must stay a *purely textual* TU: its ThreadPool
// instantiates std::condition_variable_any::wait(lock, stop_token, pred),
// whose libc++ internal helper (__atomic_unique_lock::__set_locked_bit) the
// toolchain fails to emit when the same TU also imports a module. So
// redisRunner.cpp reaches the modules only through these plain (global-module)
// functions, never naming PriceData or importing anything itself.
struct TickSliceImpl;

// A window of cached tick data. Value-copyable without naming any module type:
// pool tasks capture it BY VALUE, so the shared_ptr keeps the underlying tick
// buffer alive for the lifetime of each backtest even after the cache evicts
// or replaces it.
struct TickSlice {
    std::shared_ptr<const TickSliceImpl> impl;
    std::size_t tickCount = 0;  // logging only
};

// The ticks for one run's window, served from a per-symbols cached superset
// (loaded once, then sliced for every window that fits — the rolling ladder's
// windows all do). beforeLoad is invoked exactly once, immediately before any
// actual QuestDB load, and never on a cache hit: the drain loop quiesces its
// pool there so no in-flight backtest still reads a buffer being evicted.
TickSlice bridgeGetTicks(const std::string& questdbHost,
                         const std::string& symbolsCsv,
                         int lastMonths,
                         int offsetMonths,
                         const std::function<void()>& beforeLoad);

// Runs a single backtest against an already-loaded slice (no QuestDB access).
void bridgeRunOnTicks(const TickSlice& slice,
                      const tradingDefinitions::Configuration& config);
