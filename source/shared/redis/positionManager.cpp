// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/positionManager.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "shared/redis/client/syncRedisClient.hpp"
#include "shared/utilities/backtestLog.hpp"

namespace {

// Closed deals are kept for 60 days, matching the C# Remove().
constexpr std::chrono::hours kHistoryTtl{24 * 60};

}  // namespace

namespace redis_positions {

std::string positionListKey(const std::string& strategyId) {
    return "PL#" + strategyId;
}

std::string positionKey(const std::string& dealId) { return "PO#" + dealId; }

std::string historyPositionKey(const std::string& dealId) {
    return "PH#" + dealId;
}

std::string dealReceiptKey(const std::string& dealReference,
                           const std::string& symbol) {
    return "DealId#" + dealReference + "#" + symbol;
}

std::string encodePositionList(const std::vector<std::string>& dealIds) {
    return nlohmann::json(dealIds).dump();
}

std::optional<std::vector<std::string>> decodePositionList(
    const std::string& json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!parsed.is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> dealIds;
    dealIds.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string()) {
            return std::nullopt;
        }
        dealIds.push_back(item.get<std::string>());
    }
    return dealIds;
}

std::optional<PositionRecord> decodePositionRecord(const std::string& json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    PositionRecord record;
    const auto readString = [&parsed](const char* key, std::string& out) {
        if (const auto it = parsed.find(key);
            it != parsed.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    const auto readInt32 = [&parsed](const char* key, std::int32_t& out) {
        if (const auto it = parsed.find(key);
            it != parsed.end() && it->is_number()) {
            out = it->get<std::int32_t>();
        }
    };
    readString("dealId", record.dealId);
    readString("dealReference", record.dealReference);
    readString("symbol", record.symbol);
    readString("epic", record.epic);
    readString("direction", record.direction);
    readString("strategyId", record.strategyId);
    readString("strategyName", record.strategyName);
    readInt32("level", record.level);
    readInt32("stopLevel", record.stopLevel);
    readInt32("limitLevel", record.limitLevel);
    if (const auto it = parsed.find("size");
        it != parsed.end() && it->is_number()) {
        record.size = it->get<double>();
    }
    if (const auto it = parsed.find("openedAt");
        it != parsed.end() && it->is_number()) {
        record.openedAtMicros = it->get<std::int64_t>();
    }
    // Only these three are load-bearing for the book sync (identity, which
    // worker owns it, and which way it points); everything else may be
    // absent from a producer-rewritten payload.
    if (record.dealReference.empty() || record.symbol.empty() ||
        record.direction.empty()) {
        return std::nullopt;
    }
    return record;
}

std::string encodePositionRecord(const PositionRecord& record) {
    // Hand-rolled to stay byte-compatible with buildPositionPayload
    // (orderChannel) — same fields, same order. Every string is a broker or
    // engine identifier (no user text), so no JSON escaping is needed, the
    // same doctrine the writer relies on.
    return std::format(
        R"({{"dealId":"{}","dealReference":"{}","symbol":"{}","epic":"{}",)"
        R"("direction":"{}","size":{},"level":{},"stopLevel":{},)"
        R"("limitLevel":{},"strategyId":"{}","strategyName":"{}",)"
        R"("openedAt":{}}})",
        record.dealId, record.dealReference, record.symbol, record.epic,
        record.direction, record.size, record.level, record.stopLevel,
        record.limitLevel, record.strategyId, record.strategyName,
        record.openedAtMicros);
}

std::optional<DealReceipt> decodeDealReceipt(const std::string& json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    DealReceipt receipt;
    const auto readString = [&parsed](const char* key, std::string& out) {
        if (const auto it = parsed.find(key);
            it != parsed.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    readString("strategyId", receipt.strategyId);
    readString("dealId", receipt.dealId);
    readString("strategyName", receipt.strategyName);
    return receipt;
}

PositionManager::PositionManager(const std::string& host, const int port)
    : client_(std::make_unique<redis_util::SyncRedisClient>(host, port)) {}

PositionManager::~PositionManager() = default;

// Expects mutex_ to be held by the calling public method.
PositionManager::ListState PositionManager::fetchList(
    const std::string& strategyId, std::vector<std::string>& dealIds) {
    const std::string key = positionListKey(strategyId);
    std::string error;
    const auto value = client_->run(client_->operations().getString(key), error);
    if (!value) {
        // An empty error means the breaker skipped the probe (the original
        // failure was already logged) — same convention as TradeLocks.
        if (!error.empty()) {
            backtest_log::error("PositionManager: reading " + key + " failed ("
                                + error + ")");
        }
        return ListState::Failed;
    }
    if (!value->has_value()) {
        return ListState::Missing;
    }
    auto decoded = decodePositionList(**value);
    if (!decoded) {
        // A corrupt list is indistinguishable from an unknown position count,
        // so it reports Failed (callers fail closed) rather than "empty".
        backtest_log::error("PositionManager: " + key
                            + " is not a JSON string array; treating the "
                              "position state as unknown");
        return ListState::Failed;
    }
    dealIds = std::move(*decoded);
    return ListState::Ok;
}

std::optional<std::vector<std::string>> PositionManager::getPositionsList(
    const std::string& strategyId) {
    const std::lock_guard<std::mutex> guard{mutex_};
    std::vector<std::string> dealIds;
    switch (fetchList(strategyId, dealIds)) {
        case ListState::Ok:
        case ListState::Missing:
            return dealIds;
        case ListState::Failed:
            break;
    }
    return std::nullopt;
}

std::optional<int> PositionManager::getPositionCount(
    const std::string& strategyId) {
    const auto payloads = getPositionPayloads(strategyId);
    if (!payloads) {
        return std::nullopt;
    }
    return static_cast<int>(payloads->size());
}

bool PositionManager::savePosition(const std::string& dealId,
                                   const std::string& payload,
                                   const std::chrono::milliseconds ttl) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = positionKey(dealId);
    std::string error;
    const auto stored = client_->run(
        client_->operations().setString(key, payload, SetWhen::Always, ttl),
        error);
    if (!stored) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: saving " + key + " failed ("
                                + error + ")");
        }
        return false;
    }
    return *stored;
}

bool PositionManager::saveDealReceipt(const std::string& dealReference,
                                      const std::string& symbol,
                                      const std::string& payload,
                                      const std::chrono::hours ttl) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = dealReceiptKey(dealReference, symbol);
    std::string error;
    const auto stored = client_->run(
        client_->operations().setString(
            key, payload, SetWhen::Always,
            std::chrono::duration_cast<std::chrono::milliseconds>(ttl)),
        error);
    if (!stored) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: saving receipt " + key
                                + " failed (" + error + ")");
        }
        return false;
    }
    return *stored;
}

bool PositionManager::addPosition(const std::string& strategyId,
                                  const std::string& dealId) {
    const std::lock_guard<std::mutex> guard{mutex_};
    std::vector<std::string> dealIds;
    if (fetchList(strategyId, dealIds) == ListState::Failed) {
        return false;
    }
    if (std::ranges::find(dealIds, dealId) != dealIds.end()) {
        return true;  // already tracked — the C# add is idempotent too
    }
    dealIds.push_back(dealId);

    const std::string key = positionListKey(strategyId);
    std::string error;
    // No TTL on the list on purpose (matching C#): it is pruned by
    // refreshPositionList as the PO# deals expire, not by its own expiry.
    const auto stored = client_->run(
        client_->operations().setString(key, encodePositionList(dealIds),
                                        SetWhen::Always, std::nullopt),
        error);
    if (!stored) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: adding " + dealId + " to "
                                + key + " failed (" + error + ")");
        }
        return false;
    }
    return *stored;
}

std::optional<std::vector<std::pair<std::string, std::string>>>
PositionManager::getPositionPayloads(const std::string& strategyId) {
    const std::lock_guard<std::mutex> guard{mutex_};

    std::vector<std::string> dealIds;
    switch (fetchList(strategyId, dealIds)) {
        case ListState::Missing:
            return std::vector<std::pair<std::string, std::string>>{};
        case ListState::Failed:
            return std::nullopt;
        case ListState::Ok:
            break;
    }
    if (dealIds.empty()) {
        return std::vector<std::pair<std::string, std::string>>{};
    }

    std::vector<std::string> keys;
    keys.reserve(dealIds.size());
    for (const std::string& dealId : dealIds) {
        keys.push_back(positionKey(dealId));
    }
    // Same MGET shape as refreshPositionList: missing keys (expired PO#) are
    // simply absent from the result, and filtering the ORIGINAL list keeps
    // the stored order. Read-only — the list itself is never rewritten here.
    std::string error;
    const auto liveDeals = client_->run(
        client_->operations().getMultiple(std::move(keys)), error);
    if (!liveDeals) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: reading payloads for "
                                + positionListKey(strategyId) + " failed ("
                                + error + ")");
        }
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> payloads;
    payloads.reserve(dealIds.size());
    for (std::string& dealId : dealIds) {
        if (const auto it = liveDeals->find(positionKey(dealId));
            it != liveDeals->end()) {
            payloads.emplace_back(std::move(dealId), it->second);
        }
    }
    return payloads;
}

bool PositionManager::refreshPositionList(const std::string& strategyId) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string listKey = positionListKey(strategyId);

    std::vector<std::string> dealIds;
    switch (fetchList(strategyId, dealIds)) {
        case ListState::Missing:
            return true;  // no list — nothing to refresh (C# skips too)
        case ListState::Failed:
            return false;
        case ListState::Ok:
            break;
    }

    std::string error;
    std::vector<std::string> survivors;
    if (!dealIds.empty()) {
        std::vector<std::string> keys;
        keys.reserve(dealIds.size());
        for (const std::string& dealId : dealIds) {
            keys.push_back(positionKey(dealId));
        }
        // MGET drops keys without values, so the surviving deals are exactly
        // the ones the broker (via the producer) still refreshes. Filtering
        // the ORIGINAL list keeps the stored order deterministic.
        const auto liveDeals = client_->run(
            client_->operations().getMultiple(std::move(keys)), error);
        if (!liveDeals) {
            if (!error.empty()) {
                backtest_log::error("PositionManager: refreshing " + listKey
                                    + " failed (" + error + ")");
            }
            return false;
        }
        for (std::string& dealId : dealIds) {
            if (liveDeals->contains(positionKey(dealId))) {
                survivors.push_back(std::move(dealId));
            }
        }
    }

    if (survivors.empty()) {
        const auto removed =
            client_->run(client_->operations().deleteKey(listKey), error);
        if (!removed && !error.empty()) {
            backtest_log::error("PositionManager: deleting emptied " + listKey
                                + " failed (" + error + ")");
        }
        return removed.has_value();
    }

    const auto stored = client_->run(
        client_->operations().setString(listKey, encodePositionList(survivors),
                                        SetWhen::Always, std::nullopt),
        error);
    if (!stored) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: rewriting " + listKey
                                + " failed (" + error + ")");
        }
        return false;
    }
    return *stored;
}

bool PositionManager::removePosition(const std::string& strategyId,
                                     const std::string& dealId) {
    const std::lock_guard<std::mutex> guard{mutex_};
    std::string error;

    // Move the deal to history: GETDEL then SET PX(60d). Not atomic like the
    // C# RENAME, but a deal that already expired is a normal case here (short
    // TTL by design) and must not surface as a Redis error; losing the
    // history copy to a crash between the two steps only costs debug data.
    const auto payload = client_->run(
        client_->operations().getDelete(positionKey(dealId)), error);
    if (!payload) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: closing "
                                + positionKey(dealId) + " failed (" + error
                                + ")");
        }
        return false;
    }
    if (payload->has_value()) {
        const auto archived = client_->run(
            client_->operations().setString(
                historyPositionKey(dealId), **payload, SetWhen::Always,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kHistoryTtl)),
            error);
        if (!archived && !error.empty()) {
            backtest_log::error("PositionManager: archiving "
                                + historyPositionKey(dealId) + " failed ("
                                + error + ")");
        }
    }

    std::vector<std::string> dealIds;
    switch (fetchList(strategyId, dealIds)) {
        case ListState::Missing:
            return true;  // no list — nothing to remove the id from
        case ListState::Failed:
            return false;
        case ListState::Ok:
            break;
    }
    std::erase(dealIds, dealId);

    // Written back even when unchanged or empty, matching the C# Remove();
    // refreshPositionList deletes an emptied list on its next pass.
    const auto stored = client_->run(
        client_->operations().setString(positionListKey(strategyId),
                                        encodePositionList(dealIds),
                                        SetWhen::Always, std::nullopt),
        error);
    if (!stored) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: removing " + dealId
                                + " from " + positionListKey(strategyId)
                                + " failed (" + error + ")");
        }
        return false;
    }
    return *stored;
}

std::optional<std::optional<std::string>> PositionManager::getPositionPayload(
    const std::string& dealId) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = positionKey(dealId);
    std::string error;
    auto value = client_->run(client_->operations().getString(key), error);
    if (!value) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: reading " + key + " failed ("
                                + error + ")");
        }
        return std::nullopt;
    }
    return std::move(*value);
}

std::optional<std::optional<std::string>>
PositionManager::getHistoryPositionPayload(const std::string& dealId) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = historyPositionKey(dealId);
    std::string error;
    auto value = client_->run(client_->operations().getString(key), error);
    if (!value) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: reading " + key + " failed ("
                                + error + ")");
        }
        return std::nullopt;
    }
    return std::move(*value);
}

std::optional<std::string> PositionManager::getDealReceipt(
    const std::string& dealReference, const std::string& symbol) {
    const std::lock_guard<std::mutex> guard{mutex_};
    const std::string key = dealReceiptKey(dealReference, symbol);
    std::string error;
    const auto value = client_->run(client_->operations().getString(key), error);
    if (!value) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: reading receipt " + key
                                + " failed (" + error + ")");
        }
        return std::nullopt;
    }
    return *value;  // inner nullopt (missing receipt) flattens to nullopt
}

std::optional<std::vector<std::string>> PositionManager::listStrategyIds() {
    const std::lock_guard<std::mutex> guard{mutex_};
    static constexpr std::string_view kPrefix = "PL#";
    std::string error;
    auto keys = client_->run(
        client_->operations().getKeysByPattern(std::string{kPrefix} + "*"),
        error);
    if (!keys) {
        if (!error.empty()) {
            backtest_log::error("PositionManager: scanning PL#* failed ("
                                + error + ")");
        }
        return std::nullopt;
    }
    std::vector<std::string> strategyIds;
    strategyIds.reserve(keys->size());
    for (std::string& key : *keys) {
        strategyIds.push_back(key.substr(kPrefix.size()));
    }
    std::ranges::sort(strategyIds);
    return strategyIds;
}

}  // namespace redis_positions
