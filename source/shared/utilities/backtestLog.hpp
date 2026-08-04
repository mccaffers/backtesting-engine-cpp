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
//
// Optional sink: live mode registers a shipping callback (liveTrace ->
// elastic::enqueueDocument, index "live-logs") so every error()/logLine()
// also lands in Elasticsearch. The sink is a plain function pointer behind
// an atomic — disarmed (nullptr) for backtests and tests, where sweep-scale
// log volume must never reach an index. The thread-local suppression exists
// for the publisher's OWN threads and delivery paths: a publisher failure
// line that re-entered the publisher's queue would feed every subsequent
// flush a fresh document for as long as an outage lasts (the same doctrine
// as DocumentBatcher::flush's no-putEngineException rule).
namespace backtest_log {

// Registered shipping callback: isError distinguishes error() (stderr) from
// logLine() (stdout). Capture-free function pointer so the atomic stays
// lock-free and header-only.
using Sink = void (*)(bool isError, std::string_view message);

// Hide the actual state inside a private detail namespace.
namespace detail {
inline std::atomic<bool>& quiet_flag() {
    static std::atomic<bool> q{false};
    return q;
}

inline std::atomic<Sink>& sink_slot() {
    static std::atomic<Sink> s{nullptr};
    return s;
}

// Per-thread opt-out — see the header comment. A plain function returning a
// thread_local reference keeps this header-only and ODR-safe.
inline bool& sink_suppressed() {
    thread_local bool suppressed = false;
    return suppressed;
}
}  // namespace detail

// nullptr disarms. Release/acquire pairing so the sink's referenced state
// (liveTrace's cached env/hostname) is visible to whichever thread ships.
inline void setSink(Sink sink) {
    detail::sink_slot().store(sink, std::memory_order_release);
}

// Whether a sink is registered — for tests pinning the arm/clear gates
// (whether shipping actually happens is unobservable from outside once the
// publisher drops the document).
[[nodiscard]] inline bool sinkArmed() {
    return detail::sink_slot().load(std::memory_order_acquire) != nullptr;
}

// RAII per-thread suppression for the publisher's delivery paths (and any
// other code whose log lines must not re-enter the publisher's queue).
struct SinkSuppression {
    SinkSuppression() : previous_(detail::sink_suppressed()) {
        detail::sink_suppressed() = true;
    }
    ~SinkSuppression() { detail::sink_suppressed() = previous_; }
    SinkSuppression(const SinkSuppression&) = delete;
    SinkSuppression& operator=(const SinkSuppression&) = delete;

private:
    bool previous_;
};

// Hand `message` to the registered sink, unless disarmed or this thread is
// suppressed. Shared by error() below and the module's logLine().
inline void shipToSink(const bool isError, const std::string_view message) {
    if (detail::sink_suppressed()) {
        return;
    }
    if (const Sink sink = detail::sink_slot().load(std::memory_order_acquire)) {
        sink(isError, message);
    }
}

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
    {
        std::scoped_lock lock(errorMutex());
        std::cerr << timestamp() << ' ' << message << std::endl;
    }
    // Outside the mutex: the sink enqueues into the elastic batcher (its own
    // lock), and stderr ordering is already settled above.
    shipToSink(true, message);
}

}  // namespace backtest_log

#endif  // UTILITIES_BACKTEST_LOG_HPP
