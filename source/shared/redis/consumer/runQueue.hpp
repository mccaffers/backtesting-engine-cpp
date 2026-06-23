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
// redisRunner.cpp leaves that TU focused on orchestration (drainRuns + run()).
namespace run_queue {

// Non-destructively reads the run that RPOP would take (the queue tail, i.e. the
// oldest run since LoadCommand LPUSHes onto the head). Multiple workers all
// observe the same run and pile onto it. nullopt when the run queue is empty.
boost::asio::awaitable<std::optional<std::string>> peekRunTail(
    std::shared_ptr<boost::redis::connection> conn);

// Claims one strategy off the run's per-RUN_ID list. nullopt once drained.
boost::asio::awaitable<std::optional<std::string>> popStrategy(
    std::shared_ptr<boost::redis::connection> conn,
    std::string strategyKey);

// Retires a run by removing its descriptor. Idempotent: LREM removes 0 if a peer
// worker already retired it.
boost::asio::awaitable<void> removeRun(
    std::shared_ptr<boost::redis::connection> conn,
    std::string descriptorB64);

}  // namespace run_queue
