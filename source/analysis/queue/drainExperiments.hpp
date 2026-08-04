// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <memory>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>

// The analysis worker's per-run drain loop, split out of analysisRunner.cpp so
// that TU stays focused on orchestration — drainRuns' structure. Like
// drainRuns this is a purely textual unit: it uses ThreadPool, whose
// std::condition_variable_any::wait instantiation the toolchain miscompiles
// when the same TU also imports a module, so it reaches the tick loader and
// the chain matcher only through analysisBridge.hpp's global-module functions
// and imports nothing itself.
namespace analysis_runner {

// Drains the experiment run queue (queue_keys::EXPERIMENT_RUN — one queue, no
// priority ladder) on a single long-lived connection. Each run's ticks are
// loaded from QuestDB once (no cache — every experiment run is its own
// one-window load) and shared by value with every pooled evaluation; the
// run's experiment list is drained onto the pool, one aggregate Elasticsearch
// document per experiment. When the queue is empty it waits and re-peeks
// rather than exiting, so the worker stays up as a daemon (Ctrl+C to stop —
// deliberately no shm stop-channel in v1); only a Redis/DB/decode error
// leaves the loop (return 3).
boost::asio::awaitable<int> drainExperiments(
    std::shared_ptr<boost::redis::connection> conn,
    std::string questdbHost);

}  // namespace analysis_runner
