// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>

#include "shared/redis/operations/redisOperations.hpp"

// Synchronous, deadline-bounded facade over one Boost.Redis connection: an
// io_context the calling thread pumps, a connection kept alive by a detached
// async_run, the RedisOperations command layer, and a fail-fast circuit
// breaker. Grown out of the live trade-lock gate and shared here so every
// synchronous Redis consumer (the phase-2 broker position reader next) gets
// the same semantics instead of re-implementing the pump:
//
//  - DEADLINE-BOUNDED: with Redis down, async_run sits in a reconnect loop
//    and async_exec waits indefinitely for a connection — unbounded, that
//    would wedge the calling thread instead of letting it fail. run()
//    abandons the operation at the deadline.
//  - FAIL-FAST BREAKER: after any failure, calls within the cooldown window
//    return nullopt immediately (with an EMPTY error, so callers don't log
//    a line per skipped probe) rather than paying the timeout again.
//  - CANCEL ON FAILURE: an abandoned request stays queued inside Boost.Redis
//    and would still execute when the connection comes back; every breaker
//    trip cancels pending requests so nothing fires late (see tripBreaker).
//
// NOT thread-safe: one instance serves one caller at a time. Callers sharing
// an instance across threads must bring their own mutex (see TradeLocks);
// better, give each thread its own instance (see RedisTradeGate).
//
// This header pulls in Asio and Boost.Redis, so unlike tradeLocks.hpp it must
// stay OUT of module global module fragments — include it from plain .cpp
// implementation files only.
namespace redis_util {

class SyncRedisClient {
public:
    // Defaults: a 2s op deadline is generous for a localhost round trip but
    // short enough that a caller blocked on a dead Redis fails quickly; a 3s
    // cooldown stops a caller that retries at tick rate from paying the full
    // deadline serially while Redis is down.
    SyncRedisClient(
        const std::string& host, int port,
        std::chrono::seconds opTimeout = std::chrono::seconds{2},
        std::chrono::seconds failFastCooldown = std::chrono::seconds{3});
    ~SyncRedisClient();  // cancels the detached async_run and drains the context

    SyncRedisClient(const SyncRedisClient&) = delete;
    SyncRedisClient& operator=(const SyncRedisClient&) = delete;

    // Command builders for run(), e.g.
    //   client.run(client.operations().setIfNotExists(key, value, ttl), error)
    // The RedisOperations object is a member on purpose: coroutine frames
    // capture its `this`, and an abandoned (timed-out) frame resumes during a
    // LATER call's pump — the object it points back into must still be alive.
    RedisOperations& operations() { return ops_; }

    // Runs one awaitable Redis operation to completion or deadline. Outcomes:
    // the operation's own result, or nullopt when the breaker is open (empty
    // `error`), the op timed out, the connection died, or the coroutine threw
    // (reason in `error`). On timeout the coroutine is abandoned, so its
    // completion state lives on the heap (shared_ptr) — a stale completion
    // firing during a later call's pump writes into surviving storage, never
    // a dead stack frame.
    template <class T>
    std::optional<T> run(boost::asio::awaitable<T> op, std::string& error) {
        static_assert(!std::is_void_v<T>,
                      "run() needs a value-returning operation; wrap a void "
                      "op in a coroutine that co_returns a flag");
        if (breakerOpen()) {
            error.clear();
            return std::nullopt;
        }
        struct OpState {
            bool done = false;
            std::optional<T> result;
            std::exception_ptr failure;
        };
        auto state = std::make_shared<OpState>();
        boost::asio::co_spawn(ioc_, std::move(op),
                              [state](const std::exception_ptr e, T value) {
                                  state->failure = e;
                                  state->result = std::move(value);
                                  state->done = true;
                              });
        if (!pumpUntil(state->done, error)) {
            return std::nullopt;
        }
        if (state->failure) {
            noteFailure(state->failure, error);
            return std::nullopt;
        }
        closeBreaker();
        return std::move(state->result);
    }

private:
    [[nodiscard]] bool breakerOpen() const;
    void closeBreaker();

    // Pumps the io_context until `done` flips or the deadline passes; trips
    // the breaker on timeout or a dead connection and returns false.
    bool pumpUntil(const bool& done, std::string& error);

    // Decodes a completed operation's exception into `error` and trips.
    void noteFailure(std::exception_ptr failure, std::string& error);

    void tripBreaker(std::string reason, std::string& error);

    std::chrono::seconds opTimeout_;
    std::chrono::seconds failFastCooldown_;
    boost::asio::io_context ioc_;
    std::shared_ptr<boost::redis::connection> conn_;
    RedisOperations ops_;
    std::chrono::steady_clock::time_point failFastUntil_{};  // epoch = closed
};

}  // namespace redis_util
