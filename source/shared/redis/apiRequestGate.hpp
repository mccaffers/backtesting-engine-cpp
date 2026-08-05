// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace redis_util {
class SyncRedisClient;
}

// Redis-backed broker API request bookkeeping, mirroring the C# engine's
// IGMarketRequests counters. Two key families, shared with the C# engine so
// both processes draw on ONE request budget against the broker's rate limit:
//
//   REQ#<minute>   -> requests made in that wall-clock minute (0-59), value a
//                     plain integer string, 1-minute TTL. The window budget.
//   API#<dealId>   -> short-lived marker (30s) that a request for this deal
//                     key was just made; a second request while the marker
//                     lives is a duplicate and must not be sent (prevents
//                     accidental flooding of the same strategy+direction).
//
// Same isolation pattern as tradeLocks.hpp: Asio-free header, the connection
// machinery lives behind apiRequestGate.cpp, so module global module
// fragments can #include this alongside `import std`.
namespace redis_api {

// Key builders — free functions so tests can pin the wire formats without a
// Redis server (drift would silently split the request budget between the
// two engines).
std::string requestWindowKey(int minuteOfHour);           // "REQ#<minute>"
std::string dealRequestKey(const std::string& dealId);    // "API#<dealId>"

class ApiRequestGate {
public:
    // Connects lazily on first use via the shared Boost.Redis connection.
    ApiRequestGate(const std::string& host, int port);
    ~ApiRequestGate();
    ApiRequestGate(const ApiRequestGate&) = delete;
    ApiRequestGate& operator=(const ApiRequestGate&) = delete;

    // GET REQ#<minute>, parsed as an integer (0 when the key is missing or
    // holds garbage — matching the C# int.Parse-of-null -> 0 behaviour via
    // its null guard). nullopt means UNKNOWN (Redis unreachable): callers
    // gating outbound broker requests should fail closed and not send.
    std::optional<int> requestsMade(int minuteOfHour);

    // SET REQ#<minute> = current+1, PX 1 minute (the C# read-modify-write —
    // benign raciness accepted there and here; the budget is a soft brake,
    // the broker enforces the hard one). Returns false when Redis was
    // unreachable.
    bool recordRequest(int minuteOfHour);

    // GET API#<dealId>: true when a request for this deal key is already in
    // its suppression window, false when it is not, and nullopt when Redis
    // was unreachable (UNKNOWN). The fail-closed/fail-open decision belongs
    // to the caller: opens must treat unknown as duplicate (an unverifiable
    // duplicate order is the exact accident this marker prevents), while a
    // risk-reducing close must still go out (see igRequests' gatePolicy).
    // Empty dealId -> false (nothing to deduplicate, C# behaviour).
    std::optional<bool> isDuplicateDeal(const std::string& dealId);

    // SET API#<dealId> PX(ttl). Empty dealId is a no-op (the C# random-key
    // fallback recorded nothing useful — a key nobody will ever look up).
    // Returns false when Redis was unreachable.
    bool recordDealRequest(const std::string& dealId,
                           std::chrono::seconds ttl = std::chrono::seconds{30});

private:
    std::unique_ptr<redis_util::SyncRedisClient> client_;
    // Serialises concurrent callers sharing one instance (one connection,
    // one synchronous pump) — same pattern as TradeLocks; prefer one
    // instance per thread (see IGMarketRequests' thread_local binder).
    std::mutex mutex_;
};

}  // namespace redis_api
