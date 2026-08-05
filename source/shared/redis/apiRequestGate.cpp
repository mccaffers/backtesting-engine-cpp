// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/apiRequestGate.hpp"

#include <charconv>
#include <chrono>
#include <string>

#include "shared/redis/client/syncRedisClient.hpp"
#include "shared/utilities/backtestLog.hpp"

namespace redis_api {

std::string requestWindowKey(const int minuteOfHour) {
    return "REQ#" + std::to_string(minuteOfHour);
}

std::string dealRequestKey(const std::string& dealId) {
    return "API#" + dealId;
}

ApiRequestGate::ApiRequestGate(const std::string& host, const int port)
    : client_(std::make_unique<redis_util::SyncRedisClient>(host, port)) {}

ApiRequestGate::~ApiRequestGate() = default;

std::optional<int> ApiRequestGate::requestsMade(const int minuteOfHour) {
    const std::lock_guard<std::mutex> guard{mutex_};
    std::string error;
    const auto value = client_->run(
        client_->operations().getString(requestWindowKey(minuteOfHour)),
        error);
    if (!value) {
        // Distinguish "reached Redis, key missing" (a fresh minute — zero
        // requests) from "could not reach Redis" (unknown; callers fail
        // closed). run() yields nullopt only on failure; a missing key is a
        // present optional holding nullopt.
        if (!error.empty()) {
            backtest_log::error("ApiRequestGate: reading "
                                + requestWindowKey(minuteOfHour) + " failed ("
                                + error + ")");
        }
        return std::nullopt;
    }
    if (!value->has_value()) {
        return 0;
    }
    int count = 0;
    const std::string& text = **value;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), count);
    if (ec != std::errc{} || count < 0) {
        return 0;  // garbage in the window key counts as an empty window
    }
    return count;
}

bool ApiRequestGate::recordRequest(const int minuteOfHour) {
    const std::optional<int> current = requestsMade(minuteOfHour);
    if (!current) {
        return false;
    }
    const std::lock_guard<std::mutex> guard{mutex_};
    std::string error;
    // Unconditional SET with a fresh 1-minute TTL — the C# AddRequest. The
    // TTL restarting on every write is fine: the key names its own minute,
    // so it only needs to outlive that minute, and one extra minute of
    // stale count in a key nobody reads any more is harmless.
    const auto stored = client_->run(
        client_->operations().setString(requestWindowKey(minuteOfHour),
                                        std::to_string(*current + 1),
                                        SetWhen::Always,
                                        std::chrono::minutes{1}),
        error);
    if (!stored || !*stored) {
        if (!error.empty()) {
            backtest_log::error("ApiRequestGate: recording request in "
                                + requestWindowKey(minuteOfHour) + " failed ("
                                + error + ")");
        }
        return false;
    }
    return true;
}

std::optional<bool> ApiRequestGate::isDuplicateDeal(const std::string& dealId) {
    if (dealId.empty()) {
        return false;
    }
    const std::lock_guard<std::mutex> guard{mutex_};
    std::string error;
    const auto value = client_->run(
        client_->operations().getString(dealRequestKey(dealId)), error);
    if (!value) {
        // UNKNOWN: Redis unreachable. Report it as such — the caller's
        // policy decides (opens fail closed, risk-reducing closes fail open).
        if (!error.empty()) {
            backtest_log::error("ApiRequestGate: duplicate check for "
                                + dealRequestKey(dealId) + " failed (" + error
                                + "); reporting unknown");
        }
        return std::nullopt;
    }
    return value->has_value();
}

bool ApiRequestGate::recordDealRequest(const std::string& dealId,
                                       const std::chrono::seconds ttl) {
    if (dealId.empty()) {
        return true;
    }
    const std::lock_guard<std::mutex> guard{mutex_};
    std::string error;
    const auto stored = client_->run(
        client_->operations().setString(
            dealRequestKey(dealId), "1", SetWhen::Always,
            std::chrono::duration_cast<std::chrono::milliseconds>(ttl)),
        error);
    if (!stored || !*stored) {
        if (!error.empty()) {
            backtest_log::error("ApiRequestGate: recording "
                                + dealRequestKey(dealId) + " failed (" + error
                                + ")");
        }
        return false;
    }
    return true;
}

}  // namespace redis_api
