// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <boost/redis/connection.hpp>

// Redis queue access for the runner's drain loop. These coroutines issue one
// command each on the shared, long-lived connection and never cancel it — the
// connection stays open for the rest of the worker loop. Keeping them out of
// the drain loop (see drainRuns.cpp) keeps each TU small and single-purpose.
namespace run_queue {

// Non-destructively reads the run that RPOP would take (the queue tail, i.e. the
// oldest run since LoadCommand LPUSHes onto the head). Multiple workers all
// observe the same run and pile onto it. nullopt when the run queue is empty.
boost::asio::awaitable<std::optional<std::string>> peekRunTail(
    std::shared_ptr<boost::redis::connection> conn);

// Claims one strategy payload KEY NAME off the run's per-RUN_ID list (the list
// carries names, not payloads — see queueKeys.hpp). RPOP hands each name to
// exactly one consumer. nullopt once drained.
boost::asio::awaitable<std::optional<std::string>> popStrategyKey(
    std::shared_ptr<boost::redis::connection> conn,
    std::string strategyKey);

// Atomically takes (GETDEL) the payload stored under a popped key name, so the
// payload is consumed exactly once and nothing is left behind. nullopt when the
// key is gone — already consumed by a peer or reaped by its safety-net TTL.
boost::asio::awaitable<std::optional<std::string>> takeStrategyPayload(
    std::shared_ptr<boost::redis::connection> conn,
    std::string payloadKey);

// Retires a run by removing its descriptor. Idempotent: LREM removes 0 if a peer
// worker already retired it.
boost::asio::awaitable<void> removeRun(
    std::shared_ptr<boost::redis::connection> conn,
    std::string descriptorB64);

}  // namespace run_queue
