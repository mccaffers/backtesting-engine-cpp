// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#ifndef UTILITIES_BACKTEST_LOG_HPP
#define UTILITIES_BACKTEST_LOG_HPP

#include <atomic>
#include <chrono>
#include <cstdio>   // std::snprintf
#include <ctime>    // POSIX gmtime_r, std::strftime
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

// Cross-cutting logging switches for backtest execution.
//
// When backtests run concurrently (see RedisRunner's thread pool) the chatty
// per-strategy output would both interleave and race on the shared std::cout /
// std::cerr. RedisRunner calls set_quiet(true) so those call sites skip their output
// entirely (no stream access -> no race), while error() serialises the rare
// failure path behind a mutex so genuine problems still surface safely.
namespace backtest_log {

// Hide the actual state inside a private detail namespace.
namespace detail {
inline std::atomic<bool>& quiet_flag() {
    static std::atomic<bool> q{false};
    return q;
}
}  // namespace detail

// Provide clean, explicit public getters and setters.
inline bool is_quiet() {
    return detail::quiet_flag().load(std::memory_order_relaxed);
}

inline void set_quiet(bool state) {
    detail::quiet_flag().store(state, std::memory_order_relaxed);
}

inline std::mutex& errorMutex() {
    static std::mutex m;
    return m;
}

// UTC wall-clock prefix, e.g. "[2026-06-28 15:04:20.785]", to millisecond
// precision. gmtime_r + strftime (rather than C++23 std::format) mirrors the
// classic style in elasticPublisher.cpp and keeps this header light enough to
// stay #includable from module global module fragments.
inline std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const long millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000;
    std::tm utc{};
    gmtime_r(&t, &utc);
    char secs[20];  // "YYYY-MM-DD HH:MM:SS" + NUL
    std::strftime(secs, sizeof(secs), "%Y-%m-%d %H:%M:%S", &utc);
    char out[28];
    std::snprintf(out, sizeof(out), "[%s.%03ld]", secs, millis);
    return out;
}

inline void error(std::string_view message) {
    std::scoped_lock lock(errorMutex());
    std::cerr << timestamp() << ' ' << message << std::endl;
}

}  // namespace backtest_log

#endif  // UTILITIES_BACKTEST_LOG_HPP
