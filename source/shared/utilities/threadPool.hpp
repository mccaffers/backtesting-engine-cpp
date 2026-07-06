// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#ifndef UTILITIES_THREAD_POOL_HPP
#define UTILITIES_THREAD_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

// A small fixed-size pool for CPU-bound work, built on std::jthread.
//
// std::jthread (not std::thread) buys us three things here:
//   * Constructor exception safety: if the OS throws while spawning the Nth
//     worker, unwinding the vector auto-request_stop()s and joins the workers
//     already started. std::thread would hit un-joined threads -> terminate().
//   * A trivial destructor: ~std::jthread does request_stop() then join(), so
//     there is no manual stop flag, signal, or join loop to maintain.
//   * Native cancellation: each worker takes a std::stop_token and waits with
//     condition_variable_any, so a stop request wakes the wait directly.
//
// submit() applies backpressure: it blocks once `capacity_` tasks are in flight
// (queued + executing). This bounds memory and paces the producer to the
// workers. wait() blocks until everything has finished and never throws, so it
// is safe to call during stack unwinding (e.g. from a destructor). The first
// exception thrown by any task is captured and surfaced via takeError().
class ThreadPool {
public:
    // `inFlightGauge`, if non-null, is updated (under the lock) every time the
    // in-flight count changes, so an observer — e.g. a shared-memory monitor —
    // sees the live queued+executing total without coupling the pool to it.
    explicit ThreadPool(std::size_t threads,
                        std::atomic<std::int32_t>* inFlightGauge = nullptr)
        : capacity_(threads * 2), inFlightGauge_(inFlightGauge) {
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this](std::stop_token st) { workerLoop(st); });
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task, blocking while `capacity_` tasks are already in flight.
    void submit(std::function<void()> task) {
        std::unique_lock lock(mutex_);
        slotFree_.wait(lock, [this] { return inFlight_ < capacity_; });
        tasks_.push(std::move(task));
        ++inFlight_;
        publishInFlight();
        workAvailable_.notify_one();
    }

    // Block until no task is queued or executing. Never throws.
    void wait() {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return inFlight_ == 0; });
    }

    // Returns and clears the first exception captured from a task, if any.
    std::exception_ptr takeError() {
        std::scoped_lock lock(mutex_);
        return std::exchange(firstError_, nullptr);
    }

private:
    // Mirror the current in-flight count to the observer gauge, if one was given.
    // Always called with mutex_ held.
    void publishInFlight() {
        if (inFlightGauge_) {
            inFlightGauge_->store(static_cast<std::int32_t>(inFlight_),
                                  std::memory_order_release);
        }
    }

    void workerLoop(std::stop_token st) {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                workAvailable_.wait(lock, st, [this] { return !tasks_.empty(); });
                if (tasks_.empty()) {
                    return;  // woken by a stop request with nothing left to do
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            std::exception_ptr err;
            try {
                task();
            } catch (...) {
                err = std::current_exception();
            }

            {
                std::scoped_lock lock(mutex_);
                if (err && !firstError_) {
                    firstError_ = err;
                }
                --inFlight_;
                publishInFlight();
                if (inFlight_ == 0) {
                    idle_.notify_all();
                }
            }
            slotFree_.notify_one();
        }
    }

    std::mutex mutex_;
    std::condition_variable_any workAvailable_;  // workers wait here (stop-aware)
    std::condition_variable slotFree_;           // submit() waits here
    std::condition_variable idle_;               // wait() waits here
    std::queue<std::function<void()>> tasks_;
    std::size_t inFlight_ = 0;
    std::size_t capacity_;
    std::atomic<std::int32_t>* inFlightGauge_ = nullptr;
    std::exception_ptr firstError_;

    // Declared last so the jthreads stop and join before the synchronisation
    // members they touch above are destroyed.
    std::vector<std::jthread> workers_;
};

#endif  // UTILITIES_THREAD_POOL_HPP
