// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/tradeLocks.hpp"

#include <unistd.h>  // gethostname

#include <optional>
#include <string>

#include "shared/redis/client/syncRedisClient.hpp"
#include "shared/utilities/backtestLog.hpp"

namespace {

// Lock keys record who held them (aids debugging a stuck lock from
// redis-cli); the value plays no part in the NX semantics.
std::string lockOwner() {
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0) {
        return "live";
    }
    buf[sizeof(buf) - 1] = '\0';  // POSIX leaves truncation unspecified
    return buf;
}

}  // namespace

namespace redis_locks {

std::string lockKey(const std::string& strategyUuid,
                    const std::string& direction) {
    return "LOCK#" + strategyUuid + "#" + direction;
}

TradeLocks::TradeLocks(const std::string& host, const int port)
    : client_(std::make_unique<redis_util::SyncRedisClient>(host, port)) {}

TradeLocks::~TradeLocks() = default;

bool TradeLocks::isThereATradeLock(const std::string& strategyUuid,
                                   const std::string& direction,
                                   const std::chrono::seconds ttl) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = lockKey(strategyUuid, direction);
    const auto now = std::chrono::steady_clock::now();

    // A lock we acquired is held at least until the TTL we stored it with
    // runs out — `now` is taken BEFORE the SET, so the cached instant can
    // only under-estimate Redis's expiry, never overshoot it. While it is
    // known-held, answer locally instead of paying a round trip per signal.
    // (An external early release — e.g. a manual redis-cli DEL — goes
    // unnoticed until the cached expiry; that can only delay an entry, never
    // ungate one.)
    if (const auto it = heldUntil_.find(key); it != heldUntil_.end()) {
        if (now < it->second) {
            return true;
        }
        heldUntil_.erase(it);
    }

    std::string error;
    // SET NX PX: true = key was absent and is now ours (no pre-existing lock).
    const std::optional<bool> stored = client_->run(
        client_->operations().setIfNotExists(
            key, lockOwner(),
            std::chrono::duration_cast<std::chrono::milliseconds>(ttl)),
        error);
    if (!stored) {
        // Fail-closed: with Redis unreachable the lock state is unknowable, so
        // report "locked" — blocking an entry is recoverable, an ungated order
        // is not. An empty error means the breaker skipped the probe (the
        // original failure was already logged).
        if (!error.empty()) {
            backtest_log::error("TradeLocks: lock check failed for " + key
                                + " (" + error + "); failing closed");
        }
        return true;
    }
    if (*stored) {
        heldUntil_[key] = now + ttl;
        return false;
    }
    return true;
}

bool TradeLocks::extendLock(const std::string& strategyUuid,
                            const std::string& direction,
                            const std::chrono::seconds ttl) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = lockKey(strategyUuid, direction);
    const auto now = std::chrono::steady_clock::now();

    std::string error;
    // Unconditional SET PX — no NX: extending must succeed whether or not the
    // old TTL already lapsed, exactly like the C# extend.
    const std::optional<bool> stored = client_->run(
        client_->operations().setString(
            key, lockOwner(),
            SetWhen::Always,
            std::chrono::duration_cast<std::chrono::milliseconds>(ttl)),
        error);
    if (!stored || !*stored) {
        if (!error.empty()) {
            backtest_log::error("TradeLocks: extending " + key + " failed ("
                                + error + ")");
        }
        // The cached expiry (if any) still under-estimates the surviving TTL,
        // so it stays valid — leave it alone.
        return false;
    }
    heldUntil_[key] = now + ttl;
    return true;
}

bool TradeLocks::releaseLock(const std::string& strategyUuid,
                             const std::string& direction) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = lockKey(strategyUuid, direction);
    // Whatever DEL returns, this instance no longer considers the lock held:
    // dropping the cached expiry forces the next isThereATradeLock through a
    // real SET NX, which sees the truth either way (key gone -> reacquire;
    // DEL failed and key survives -> still locked).
    heldUntil_.erase(key);

    std::string error;
    const std::optional<bool> removed =
        client_->run(client_->operations().deleteKey(key), error);
    if (!removed) {
        if (!error.empty()) {
            backtest_log::error("TradeLocks: releasing " + key + " failed ("
                                + error + ")");
        }
        return false;
    }
    // DEL on an already-expired key reports false; the lock is gone either
    // way, so that is still a successful release.
    return true;
}

}  // namespace redis_locks
