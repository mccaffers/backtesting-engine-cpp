// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/redis/positionClustering.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "shared/redis/client/syncRedisClient.hpp"
#include "shared/utilities/backtestLog.hpp"

namespace redis_clusters {

static_assert(
    [] {
        for (std::size_t i = 1; i < kClusterTable.size(); ++i) {
            if (!(kClusterTable[i - 1].symbol < kClusterTable[i].symbol)) {
                return false;
            }
        }
        return true;
    }(),
    "redis_clusters::kClusterTable must stay sorted ascending by symbol — "
    "groupsFor binary-searches it");

static_assert(
    [] {
        for (const SymbolClusters& entry : kClusterTable) {
            if (entry.count == 0 || entry.count > entry.groups.size()) {
                return false;
            }
            for (std::size_t i = 0; i < entry.groups.size(); ++i) {
                if ((i < entry.count) != !entry.groups[i].empty()) {
                    return false;
                }
            }
        }
        return true;
    }(),
    "redis_clusters::kClusterTable counts must match the non-empty prefix "
    "of each groups array");

static_assert(
    [] {
        for (std::size_t i = 1; i < kClusterRules.size(); ++i) {
            if (!(kClusterRules[i - 1].cluster < kClusterRules[i].cluster)) {
                return false;
            }
        }
        return true;
    }(),
    "redis_clusters::kClusterRules must stay sorted ascending by cluster — "
    "clusterLimit/isStrictCluster binary-search it");

std::string clusterKey(const std::string_view cluster) {
    return "CG#" + std::string{cluster};
}

std::string clusterLockKey(const std::string_view cluster) {
    return "CLUSTER_LOCK#" + std::string{cluster};
}

std::span<const std::string_view> groupsFor(
    const std::string_view symbol) noexcept {
    std::size_t lo = 0;
    std::size_t hi = kClusterTable.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        const SymbolClusters& entry = kClusterTable[mid];
        if (entry.symbol < symbol) {
            lo = mid + 1;
        } else if (symbol < entry.symbol) {
            hi = mid;
        } else {
            return {entry.groups.data(), entry.count};
        }
    }
    return {};
}

namespace {

const ClusterRule* findRule(const std::string_view cluster) noexcept {
    std::size_t lo = 0;
    std::size_t hi = kClusterRules.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        const ClusterRule& rule = kClusterRules[mid];
        if (rule.cluster < cluster) {
            lo = mid + 1;
        } else if (cluster < rule.cluster) {
            hi = mid;
        } else {
            return &rule;
        }
    }
    return nullptr;
}

// "symbol#strategyName#dealReference" -> (symbol, strategyName); nullopt for
// anything with fewer than two '#'-separated fields (the C# parts.Length >= 2
// tolerance — a malformed member is skipped, never trusted).
std::optional<std::pair<std::string_view, std::string_view>> splitMember(
    const std::string_view member) {
    const std::size_t firstHash = member.find('#');
    if (firstHash == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = member.substr(firstHash + 1);
    const std::size_t secondHash = rest.find('#');
    const std::string_view strategy =
        secondHash == std::string_view::npos ? rest : rest.substr(0, secondHash);
    return std::make_pair(member.substr(0, firstHash), strategy);
}

}  // namespace

int clusterLimit(const std::string_view cluster) noexcept {
    const ClusterRule* rule = findRule(cluster);
    return rule != nullptr ? rule->limit : 1;  // the C# GroupLimits fallback
}

bool isStrictCluster(const std::string_view cluster) noexcept {
    const ClusterRule* rule = findRule(cluster);
    return rule != nullptr && rule->strict;
}

ClusterVerdict evaluateClusterGroup(const std::span<const std::string> members,
                                    const std::string_view symbol,
                                    const std::string_view strategyName,
                                    const int limit, const bool strict) {
    // A. Capacity — counts EVERY member, well-formed or not, like the C#
    // members.Length check.
    if (members.size() >= static_cast<std::size_t>(limit)) {
        return ClusterVerdict::GroupFull;
    }
    // B. Same (symbol, strategy) already open — signal stacking.
    for (const std::string& member : members) {
        if (const auto parts = splitMember(member);
            parts && parts->first == symbol && parts->second == strategyName) {
            return ClusterVerdict::SymbolStrategyStacked;
        }
    }
    // C. Strategy diversity, strict clusters only: the same strategy on two
    // correlated members is the same risk twice.
    if (strict) {
        for (const std::string& member : members) {
            if (const auto parts = splitMember(member);
                parts && parts->second == strategyName) {
                return ClusterVerdict::StrategyNotDiverse;
            }
        }
    }
    return ClusterVerdict::Allowed;
}

std::string clusterMemberString(const ClusterMember& member) {
    return member.symbol + "#" + member.strategyName + "#"
           + member.dealReference;
}

std::map<std::string, std::vector<std::string>, std::less<>>
groupMembersByCluster(const std::span<const ClusterMember> members,
                      const GroupsLookup& lookup) {
    std::map<std::string, std::vector<std::string>, std::less<>> clusters;
    for (const ClusterMember& member : members) {
        const std::string value = clusterMemberString(member);
        for (const std::string_view group : lookup(member.symbol)) {
            clusters[std::string{group}].push_back(value);
        }
    }
    // Dedup (the C# HashSet) + sort, so a rebuilt set's SADD sequence is
    // deterministic regardless of the broker's position order.
    for (auto& [group, values] : clusters) {
        std::ranges::sort(values);
        const auto duplicates = std::ranges::unique(values);
        values.erase(duplicates.begin(), duplicates.end());
    }
    return clusters;
}

namespace {

// Distinguishes this process's temp keys from another producer's mid-swap
// (the C# used a fresh Guid per swap; one nonce per process is enough here —
// calls are serialised by mutex_, so only ANOTHER process can collide, and
// only during a rollout overlap).
const std::string& processNonce() {
    static const std::string nonce = [] {
        std::random_device device;
        return std::format("{:08x}{:08x}", device(), device());
    }();
    return nonce;
}

}  // namespace

PositionClustering::PositionClustering(const std::string& host, const int port)
    : client_(std::make_unique<redis_util::SyncRedisClient>(host, port)) {}

PositionClustering::~PositionClustering() = default;

bool PositionClustering::isClusterBlocked(const std::string& symbol,
                                          const std::string& strategyName,
                                          const std::chrono::seconds lockTtl) {
    const std::lock_guard<std::mutex> guard{mutex_};

    const std::span<const std::string_view> groups = groupsFor(symbol);
    if (groups.empty()) {
        // Unmapped symbol: fail-closed, matching the C# (a symbol with no
        // risk cluster must not trade around the portfolio caps).
        backtest_log::error("PositionClustering: " + symbol
                            + " has no cluster mapping; blocking entry");
        return true;
    }

    // Phase 1 — the cooldown try-locks, all-or-nothing. A group already
    // locked means another entry is mid-check (or just passed) somewhere in
    // an overlapping cluster: back off. Locks this check DID acquire are
    // deliberately left to their short TTL rather than rolled back with a
    // DEL — the delete is value-blind, and another worker's markOpened can
    // overwrite an acquired key with the 2-minute post-open cooldown between
    // our SET NX and the rollback; deleting it would reopen the exact
    // CG#-staleness gap markOpened closes (and a same-symbol overwrite makes
    // even a value-compared delete unsafe). The cost of not rolling back is
    // bounded and safe-side: a sibling entry is over-blocked for at most
    // lockTtl seconds.
    for (const std::string_view group : groups) {
        const std::string key = clusterLockKey(group);
        std::string error;
        const std::optional<bool> stored = client_->run(
            client_->operations().setIfNotExists(
                key, symbol,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    lockTtl)),
            error);
        if (stored && *stored) {
            continue;
        }
        if (!stored && !error.empty()) {
            // Fail-closed: lock state unknowable. (Empty error = the breaker
            // skipped the probe; the original failure was already logged.)
            backtest_log::error("PositionClustering: cluster lock check "
                                "failed for " + key + " (" + error
                                + "); failing closed");
        }
        return true;
    }

    // Phase 2 — evaluate each cluster's members. On any block (or unknowable
    // membership) the cooldown locks are left to expire, quieting the
    // cluster either way — the C# does the same.
    for (const std::string_view group : groups) {
        std::string error;
        const std::optional<std::vector<std::string>> members = client_->run(
            client_->operations().setMembers(clusterKey(group)), error);
        if (!members) {
            if (!error.empty()) {
                backtest_log::error("PositionClustering: reading "
                                    + clusterKey(group) + " failed (" + error
                                    + "); failing closed");
            }
            return true;
        }
        // Rejections route through error() like every other line here: the
        // plain header offers no format logger, and a portfolio-cap block is
        // operationally notable either way. Tags mirror the C# messages.
        switch (evaluateClusterGroup(*members, symbol, strategyName,
                                     clusterLimit(group),
                                     isStrictCluster(group))) {
            case ClusterVerdict::Allowed:
                break;
            case ClusterVerdict::GroupFull:
                backtest_log::error(std::format(
                    "PositionClustering: [Limit] {} rejected — {} is full "
                    "({}/{})",
                    symbol, group, members->size(), clusterLimit(group)));
                return true;
            case ClusterVerdict::SymbolStrategyStacked:
                backtest_log::error(std::format(
                    "PositionClustering: [Stacking] {} rejected — {} already "
                    "has an open position on this symbol",
                    symbol, strategyName));
                return true;
            case ClusterVerdict::StrategyNotDiverse:
                backtest_log::error(std::format(
                    "PositionClustering: [Diversity] {} rejected — {} "
                    "already has {}",
                    symbol, group, strategyName));
                return true;
        }
    }

    return false;
}

void PositionClustering::markOpened(const std::string& symbol,
                                    const std::chrono::seconds ttl) {
    const std::lock_guard<std::mutex> guard{mutex_};

    // Unconditional SET (not NX): the deal is live at the broker, so any
    // short check-time lock a concurrent signal holds is superseded — the
    // capacity it was probing for is taken. Best-effort per key: a failure
    // leaves that cluster to the short cooldown and the producer's next
    // sync; there is no entry to refuse here, so nothing fails closed.
    for (const std::string_view group : groupsFor(symbol)) {
        const std::string key = clusterLockKey(group);
        std::string error;
        const std::optional<bool> stored = client_->run(
            client_->operations().setString(
                key, symbol, SetWhen::Always,
                std::chrono::duration_cast<std::chrono::milliseconds>(ttl)),
            error);
        if ((!stored || !*stored) && !error.empty()) {
            // Empty error = the breaker skipped the probe (already logged).
            backtest_log::error("PositionClustering: post-open cooldown for "
                                + key + " failed (" + error
                                + "); the producer's sync is the backstop");
        }
    }
}

bool PositionClustering::syncAllClusters(
    const std::span<const ClusterMember> members,
    const std::chrono::milliseconds ttl) {
    const std::lock_guard<std::mutex> guard{mutex_};

    bool allSynced = true;
    for (const auto& [cluster, values] :
         groupMembersByCluster(members, groupsFor)) {
        const std::string tempKey =
            "temp_CG#" + cluster + "#" + processNonce();
        std::string error;

        // A leftover temp key (a crash mid-swap under this same nonce) would
        // merge stale members into the rebuilt set — clear it first.
        if (!client_->run(client_->operations().deleteKey(tempKey), error)
            && !error.empty()) {
            backtest_log::error("PositionClustering: clearing " + tempKey
                                + " failed (" + error + "); skipping "
                                + cluster + " this sync");
            allSynced = false;
            continue;
        }

        bool filled = true;
        for (const std::string& value : values) {
            if (!client_->run(client_->operations().setAdd(tempKey, value),
                              error)) {
                if (!error.empty()) {
                    backtest_log::error("PositionClustering: filling "
                                        + tempKey + " failed (" + error
                                        + "); skipping " + cluster
                                        + " this sync");
                }
                filled = false;
                break;
            }
        }
        if (!filled) {
            // Best-effort cleanup; a survivor is also cleared by the DEL at
            // the start of the next sync. CG# keeps its previous contents.
            std::string cleanupError;
            client_->run(client_->operations().deleteKey(tempKey),
                         cleanupError);
            allSynced = false;
            continue;
        }

        // groupMembersByCluster never emits an empty cluster, so the temp key
        // exists here — RENAME cannot fire on a missing source.
        const std::string key = clusterKey(cluster);
        if (!client_->run(client_->operations().keyRename(tempKey, key),
                          error)) {
            if (!error.empty()) {
                backtest_log::error("PositionClustering: swapping " + tempKey
                                    + " -> " + key + " failed (" + error
                                    + ")");
            }
            allSynced = false;
            continue;
        }
        if (!client_->run(client_->operations().setTTL(key, ttl), error)) {
            // The swap landed but the expiry did not: the set is correct now
            // yet immortal if the producer dies — flag it; the next sync's
            // swap re-arms the TTL.
            if (!error.empty()) {
                backtest_log::error("PositionClustering: expiring " + key
                                    + " failed (" + error + ")");
            }
            allSynced = false;
        }
    }
    return allSynced;
}

}  // namespace redis_clusters
