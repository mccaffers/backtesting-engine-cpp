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

// The runner's per-run drain loop, split out of redisRunner.cpp so that TU stays
// focused on orchestration (run()). Like runQueue, this is a purely textual unit:
// it uses ThreadPool, whose std::condition_variable_any::wait instantiation the
// toolchain miscompiles when the same TU also imports a C++ module, so it reaches
// the backtest engine only through runnerBridge.hpp's global-module functions and
// imports nothing itself.
namespace redis_runner {

// Drains BACKTESTING_QUEUE_RUN on a single long-lived connection. For each run it
// loads the QuestDB ticks once, drains that run's strategy list, then retires the
// run. When the queue is empty it waits and re-peeks rather than exiting, so the
// worker stays up as a daemon; only a Redis/DB/decode error leaves the loop
// (return 3).
boost::asio::awaitable<int> drainRuns(
    std::shared_ptr<boost::redis::connection> conn,
    std::string questdbHost);

}  // namespace redis_runner
