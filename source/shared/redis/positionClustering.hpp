// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redis_util {
class SyncRedisClient;
}

// Redis-backed position clustering, mirroring the C# engine's
// PositionClustering.CheckForCluster — the portfolio-level entry gate that
// caps correlated exposure. Every symbol maps to one or more risk clusters
// (US_Index, USD_Forex, EUR_Pairs, ...); an entry is blocked when any of its
// clusters is at capacity, already holds this exact (symbol, strategy), or —
// for the STRICT clusters (correlated indices/commodities) — already holds
// any deal from the same strategy name.
//
// Wire contract shared with the C# ecosystem (do not drift):
//
//   CG#<cluster>           Redis SET of "symbol#strategyName#dealReference"
//                          members, rebuilt every minute by the position
//                          producer (the `positions` subcommand via
//                          syncAllClusters below — historically the external
//                          C# cron) from the broker's own book: a temp-key
//                          RENAME swap and a 5-minute TTL, so a cluster
//                          whose deals all closed simply expires. The entry
//                          gate READS the sets; syncAllClusters is the only
//                          writer.
//   CLUSTER_LOCK#<cluster> Cooldown try-lock (SET NX PX, 10s) taken on
//                          every cluster the symbol belongs to BEFORE the
//                          sets are read. Two near-simultaneous entries into
//                          overlapping clusters serialise on it, and the
//                          locks are left to expire whatever the verdict.
//                          When the broker then ACCEPTS the open, markOpened
//                          re-arms the locks for a cooldown sized to the
//                          producer's ~2-minute sync cadence, so no other
//                          member clears a capacity check against CG# sets
//                          that do not yet hold the fresh deal. A rejected
//                          or failed open arms nothing — the cluster stays
//                          on the short 10s cooldown and reopens promptly.
//
// Fail-closed doctrine (same as tradeLocks.hpp): any Redis failure — lock
// state or membership unknowable — blocks the entry. A missed entry is
// recoverable; an entry that busts a cluster cap is not. Unmapped symbols
// are blocked outright, matching the C#.
//
// Asio-free header on purpose (same isolation pattern as tradeLocks.hpp /
// positionManager.hpp): the connection machinery lives behind
// positionClustering.cpp so module global module fragments can #include
// this alongside `import std`.
namespace redis_clusters {

// Key builders — free functions so tests can pin the wire format against the
// C# producer without a Redis server.
std::string clusterKey(std::string_view cluster);      // "CG#<cluster>"
std::string clusterLockKey(std::string_view cluster);  // "CLUSTER_LOCK#<cluster>"

// One symbol's cluster memberships. `groups` is padded with empty views up
// to the widest membership (EURNOK: 4); `count` is the used prefix.
struct SymbolClusters {
    std::string_view symbol;
    std::array<std::string_view, 4> groups;
    std::size_t count;
};

// MUST stay sorted ascending by symbol (static_assert in the .cpp) —
// groupsFor binary-searches it. Mirrors the C# BuildCluster map verbatim,
// including two symbols the engine does not trade (GBPAUD, NGASCMDUSD):
// the producer may still write them into shared clusters, and dropping them
// here would silently change this side's view of the map.
inline constexpr std::array<SymbolClusters, 31> kClusterTable{{
    {"AUDNZD", {"Commodity_FX", "Crosses", "AUD_Pairs", ""}, 3},
    {"AUDUSD", {"USD_Forex", "Commodity_FX", "AUD_Pairs", ""}, 3},
    {"AUSIDXAUD", {"Asian_Index", "", "", ""}, 1},
    {"BRENTCMDUSD", {"Energy", "", "", ""}, 1},
    {"COPPERCMDUSD", {"Industrial_Metals", "", "", ""}, 1},
    {"DEUIDXEUR", {"EU_Index", "", "", ""}, 1},
    {"EURAUD", {"Crosses", "EUR_Pairs", "AUD_Pairs", ""}, 3},
    {"EURCHF", {"Crosses", "EUR_Pairs", "", ""}, 2},
    {"EURGBP", {"Crosses", "EUR_Pairs", "GBP_Pairs", ""}, 3},
    {"EURJPY", {"Crosses", "EUR_Pairs", "JPY_Pairs", ""}, 3},
    {"EURNOK", {"Commodity_FX", "Crosses", "EUR_Pairs", "Scandi_Pairs"}, 4},
    {"EURUSD", {"USD_Forex", "EUR_Pairs", "", ""}, 2},
    {"FRAIDXEUR", {"EU_Index", "", "", ""}, 1},
    {"GBPAUD", {"Crosses", "GBP_Pairs", "AUD_Pairs", ""}, 3},
    {"GBPJPY", {"Crosses", "GBP_Pairs", "JPY_Pairs", ""}, 3},
    {"GBPUSD", {"USD_Forex", "GBP_Pairs", "", ""}, 2},
    {"GBRIDXGBP", {"UK_Index", "", "", ""}, 1},
    {"HKGIDXHKD", {"Asian_Index", "", "", ""}, 1},
    {"JPNIDXJPY", {"Asian_Index", "", "", ""}, 1},
    {"LIGHTCMDUSD", {"Energy", "", "", ""}, 1},
    {"NGASCMDUSD", {"Energy", "", "", ""}, 1},
    {"NZDUSD", {"USD_Forex", "Commodity_FX", "", ""}, 2},
    {"USA30IDXUSD", {"US_Index", "", "", ""}, 1},
    {"USA500IDXUSD", {"US_Index", "", "", ""}, 1},
    {"USATECHIDXUSD", {"US_Index", "", "", ""}, 1},
    {"USDCAD", {"USD_Forex", "Commodity_FX", "", ""}, 2},
    {"USDCHF", {"USD_Forex", "", "", ""}, 1},
    {"USDJPY", {"USD_Forex", "JPY_Pairs", "", ""}, 2},
    {"USDSEK", {"USD_Forex", "Scandi_Pairs", "", ""}, 2},
    {"XAGUSD", {"Precious_Metals", "", "", ""}, 1},
    {"XAUUSD", {"Precious_Metals", "", "", ""}, 1},
}};

// Per-cluster rule: capacity, and whether the cluster enforces strategy
// DIVERSITY (strict = correlated assets where the same strategy on two
// members is the same risk twice). Mirrors the C# GroupLimits +
// StrictClusters. Sorted ascending by cluster name (static_assert in .cpp).
struct ClusterRule {
    std::string_view cluster;
    int limit;
    bool strict;
};

inline constexpr std::array<ClusterRule, 15> kClusterRules{{
    {"AUD_Pairs", 2, false},
    {"Asian_Index", 1, true},
    {"Commodity_FX", 2, false},
    {"Crosses", 2, false},
    {"EUR_Pairs", 2, false},
    {"EU_Index", 1, true},
    {"Energy", 1, true},
    {"GBP_Pairs", 2, false},
    {"Industrial_Metals", 1, true},
    {"JPY_Pairs", 2, false},
    {"Precious_Metals", 1, true},
    {"Scandi_Pairs", 1, false},
    {"UK_Index", 1, true},
    {"USD_Forex", 2, false},
    {"US_Index", 1, true},
}};

// The clusters `symbol` belongs to (a view into kClusterTable); empty when
// the symbol is unmapped — the caller must then BLOCK (C# returns true).
[[nodiscard]] std::span<const std::string_view> groupsFor(
    std::string_view symbol) noexcept;

// Capacity of a cluster; 1 for a cluster missing from kClusterRules (the C#
// GroupLimits fallback). Strictness defaults to false for unknown clusters.
[[nodiscard]] int clusterLimit(std::string_view cluster) noexcept;
[[nodiscard]] bool isStrictCluster(std::string_view cluster) noexcept;

// The decision for ONE cluster, given its current CG# members — pure logic
// (limit/strict passed in from the tables above), exported so tests can pin
// every branch without a server. Checks run in the C# order: capacity
// first, then same-(symbol, strategy) stacking, then (strict only) strategy
// diversity. Note the diversity branch is currently shadowed for the real
// tables — every strict cluster has limit 1, so any occupant reports
// GroupFull first — but it guards the day a strict cluster's limit is
// raised, exactly like the C#. Members are
// "symbol#strategyName#dealReference"; anything with fewer than two
// '#'-separated fields is skipped, like the C#.
enum class ClusterVerdict {
    Allowed,
    GroupFull,               // members >= limit
    SymbolStrategyStacked,   // this exact (symbol, strategy) already open
    StrategyNotDiverse,      // strict cluster already holds this strategy
};

[[nodiscard]] ClusterVerdict evaluateClusterGroup(
    std::span<const std::string> members, std::string_view symbol,
    std::string_view strategyName, int limit, bool strict);

// One open broker deal, as the position producer sees it — the raw material
// of a CG# member. `strategyName` (not the UUID) on purpose: the strict
// clusters' diversity rule compares strategy NAMES, and the C# collector fed
// them the same way.
struct ClusterMember {
    std::string symbol;
    std::string strategyName;
    std::string dealReference;
};

// The CG# member wire format: "symbol#strategyName#dealReference" (the shape
// splitMember/evaluateClusterGroup parse back). Free so tests pin it against
// the C# producer without a server.
std::string clusterMemberString(const ClusterMember& member);

// Lookup seam so tests can group against their own cluster table — the same
// injection pattern as live::MarketLookup. Production passes groupsFor.
using GroupsLookup =
    std::function<std::span<const std::string_view>(std::string_view)>;

// The C# SyncAllClusters grouping stage, pure: fan each member out to every
// cluster its symbol belongs to, dedup within a cluster (the C# HashSet) and
// sort for determinism. Members whose symbol is unmapped are dropped, like
// the C# TryGetValue skip — the ENTRY gate is where an unmapped symbol
// fails closed; a producer must still mirror the rest of the book.
std::map<std::string, std::vector<std::string>, std::less<>>
groupMembersByCluster(std::span<const ClusterMember> members,
                      const GroupsLookup& lookup);

class PositionClustering {
public:
    // Connects lazily on first use via the shared Boost.Redis connection
    // (host from $REDIS_HOST in the caller; port 6379 by convention).
    PositionClustering(const std::string& host, int port);
    ~PositionClustering();
    PositionClustering(const PositionClustering&) = delete;
    PositionClustering& operator=(const PositionClustering&) = delete;

    // The C# CheckForCluster: TRUE = block the entry. Acquires the
    // CLUSTER_LOCK cooldown on every cluster of `symbol` (backing off if any
    // is already held — locks it did acquire are left to their short TTL,
    // never DEL-rolled-back: a value-blind delete could destroy a concurrent
    // markOpened cooldown, see the .cpp), then evaluates each cluster's
    // CG# members. On a PASS the cooldown locks are left to expire (10s);
    // on a verdict block they are too (the losing signal already paid the
    // round trips — the cooldown quiets the cluster either way). Fail-closed
    // on every Redis failure. `lockTtl` is parameterised for tests only.
    bool isClusterBlocked(const std::string& symbol,
                          const std::string& strategyName,
                          std::chrono::seconds lockTtl =
                              std::chrono::seconds{10});

    // Called AFTER the broker accepted an open on `symbol` (HTTP 200 +
    // dealStatus resolved — the order channel's Accepted branch, never on
    // Rejected/Failed): unconditionally re-arms CLUSTER_LOCK on every
    // cluster the symbol belongs to for `ttl`. The new position will not
    // appear in CG# until the external producer's next broker-book sync
    // (~2-minute cadence), and the check-time 10s lock has usually expired
    // by then — without this hold, a different strategy in the cluster
    // clears a capacity check against sets that predate the deal. The SET
    // is unconditional (not NX): the position exists, so stomping a
    // concurrent checker's short lock only strengthens the guard.
    // Best-effort: failures log and rely on the producer sync as backstop —
    // the deal is already open, so there is nothing to fail closed FOR.
    void markOpened(const std::string& symbol,
                    std::chrono::seconds ttl = std::chrono::minutes{2});

    // The producer side (port of the C# SyncAllClusters), the class's only
    // writer: for every NON-empty cluster the members map to, SADD them into
    // a temp key and RENAME it over CG#<cluster> (an atomic swap — old
    // zombies vanish with it), then PEXPIRE `ttl`. Clusters with no current
    // members are left to expire on their previous TTL, exactly like the C#.
    // The RENAME only runs after every SADD succeeded, so it can never fire
    // on a missing temp key. Returns false when any cluster's swap failed
    // (its CG# keeps the previous contents; the next minute's sync repairs
    // it).
    bool syncAllClusters(std::span<const ClusterMember> members,
                         std::chrono::milliseconds ttl =
                             std::chrono::minutes{5});

private:
    std::unique_ptr<redis_util::SyncRedisClient> client_;
    // Serialises concurrent callers sharing one instance (one connection,
    // one synchronous pump) — prefer one instance per worker thread (see
    // brokerOrderSink::threadChannel).
    std::mutex mutex_;
};

}  // namespace redis_clusters
