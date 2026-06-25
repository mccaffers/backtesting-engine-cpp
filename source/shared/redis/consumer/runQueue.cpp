// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/consumer/runQueue.hpp"

#include <optional>
#include <string>
#include <utility>

#include <boost/asio/use_awaitable.hpp>
#include <boost/redis/connection.hpp>

#include "shared/utilities/queueKeys.hpp"

namespace asio = boost::asio;
namespace redis = boost::redis;

namespace {

// One Redis command on the shared connection, returning a bulk string (or nil).
// A nil reply (RPOP/LINDEX out of range) maps to nullopt. The connection is NOT
// cancelled here, it stays open for the rest of the worker loop.
asio::awaitable<std::optional<std::string>> execOptionalString(
    const std::shared_ptr<redis::connection> conn,
    const redis::request req) {
    redis::response<std::optional<std::string>> resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);
    co_return std::move(std::get<0>(resp).value());
}

// One Redis command on the shared connection whose reply we ignore (e.g. LREM).
asio::awaitable<void> execIgnore(std::shared_ptr<redis::connection> conn,
                                 redis::request req) {
    redis::generic_response resp;
    co_await conn->async_exec(req, resp, asio::use_awaitable);
    co_return;
}

}  // namespace

namespace run_queue {

asio::awaitable<std::optional<std::string>> peekRunTail(
    std::shared_ptr<redis::connection> conn) {
    redis::request req;
    req.push("LINDEX", queue_keys::RUN, "-1");
    co_return co_await execOptionalString(std::move(conn), std::move(req));
}

asio::awaitable<std::optional<std::string>> popStrategy(
    std::shared_ptr<redis::connection> conn,
    std::string strategyKey) {
    redis::request req;
    req.push("RPOP", strategyKey);
    co_return co_await execOptionalString(std::move(conn), std::move(req));
}

asio::awaitable<void> removeRun(std::shared_ptr<redis::connection> conn,
                                std::string descriptorB64) {
    redis::request req;
    req.push("LREM", queue_keys::RUN, "0", descriptorB64);
    co_await execIgnore(std::move(conn), std::move(req));
}

}  // namespace run_queue
