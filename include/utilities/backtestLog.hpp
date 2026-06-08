// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#ifndef UTILITIES_BACKTEST_LOG_HPP
#define UTILITIES_BACKTEST_LOG_HPP

#include <atomic>
#include <iostream>
#include <mutex>
#include <string_view>

// Cross-cutting logging switches for backtest execution.
//
// When backtests run concurrently (see RedisRunner's thread pool) the chatty
// per-strategy output would both interleave and race on the shared std::cout /
// std::cerr. RedisRunner sets `quiet` so those call sites skip their output
// entirely (no stream access -> no race), while error() serialises the rare
// failure path behind a mutex so genuine problems still surface safely.
namespace backtest_log {

inline std::atomic<bool> quiet{false};

inline std::mutex& errorMutex() {
    static std::mutex m;
    return m;
}

inline void error(std::string_view message) {
    std::scoped_lock lock(errorMutex());
    std::cerr << message << std::endl;
}

}  // namespace backtest_log

#endif  // UTILITIES_BACKTEST_LOG_HPP
