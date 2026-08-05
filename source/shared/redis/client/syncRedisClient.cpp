// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/client/syncRedisClient.hpp"

#include <boost/redis/operation.hpp>

#include "shared/redis/connection/redisConnection.hpp"

namespace redis_util {

SyncRedisClient::SyncRedisClient(const std::string& host, const int port,
                                 const std::chrono::seconds opTimeout,
                                 const std::chrono::seconds failFastCooldown)
    : opTimeout_(opTimeout),
      failFastCooldown_(failFastCooldown),
      conn_(makeRedisConnection(ioc_, host, port)),
      ops_(conn_) {}

SyncRedisClient::~SyncRedisClient() {
    // Cancel the connection's detached async_run, then drain the context so
    // it winds down cleanly before the io_context is destroyed.
    conn_->cancel();
    ioc_.restart();
    ioc_.run();
}

bool SyncRedisClient::breakerOpen() const {
    return std::chrono::steady_clock::now() < failFastUntil_;
}

void SyncRedisClient::closeBreaker() { failFastUntil_ = {}; }

bool SyncRedisClient::pumpUntil(const bool& done, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + opTimeout_;
    ioc_.restart();
    while (!done) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            tripBreaker("timed out (Redis unreachable?)", error);
            return false;
        }
        if (ioc_.run_one_for(deadline - now) == 0 && !done) {
            // 0 means the wait timed out OR the context ran out of work
            // (async_run died); disambiguate by the clock.
            tripBreaker(
                std::chrono::steady_clock::now() >= deadline
                    ? "timed out (Redis unreachable?)"
                    : "connection stopped before the command completed",
                error);
            return false;
        }
    }
    return true;
}

void SyncRedisClient::noteFailure(const std::exception_ptr failure,
                                  std::string& error) {
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& e) {
        tripBreaker(e.what(), error);
    } catch (...) {
        tripBreaker("unknown error", error);
    }
}

void SyncRedisClient::tripBreaker(std::string reason, std::string& error) {
    // A request issued while disconnected stays queued inside Boost.Redis
    // (cancel_if_not_connected is off by default) and would execute when the
    // connection comes back — for a SET NX gate that would mint a phantom
    // lock for an order that was never placed, blocking re-entry for a full
    // TTL after an outage. Cancel whatever is still pending so a failed call
    // leaves nothing behind to fire late. Harmless when nothing is queued.
    conn_->cancel(boost::redis::operation::exec);
    failFastUntil_ = std::chrono::steady_clock::now() + failFastCooldown_;
    error = std::move(reason);
}

}  // namespace redis_util
