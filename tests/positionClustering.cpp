// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// redis_clusters — the C# PositionClustering port. Pins the wire contract
// shared with the C# producer (CG#/CLUSTER_LOCK# key formats, the
// symbol -> cluster map, per-cluster limits and strictness) and the pure
// evaluateClusterGroup verdict logic, all without a Redis server — the same
// convention as the tradeLocks/positionManager tests.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shared/redis/positionClustering.hpp"

import symbolScale;

namespace {

using redis_clusters::ClusterVerdict;

std::vector<std::string_view> groupsVector(const std::string_view symbol) {
    const auto groups = redis_clusters::groupsFor(symbol);
    return {groups.begin(), groups.end()};
}

}  // namespace

TEST_CASE("cluster keys match the C# PositionClustering formats",
          "[positionClustering]") {
    CHECK(redis_clusters::clusterKey("US_Index") == "CG#US_Index");
    CHECK(redis_clusters::clusterLockKey("US_Index")
          == "CLUSTER_LOCK#US_Index");
}

TEST_CASE("every priced symbol belongs to at least one cluster",
          "[positionClustering]") {
    // A symbol the engine can trade but no cluster knows about would be
    // blocked outright by the fail-closed gate — that must be a deliberate
    // table edit, not an accident.
    for (const auto& entry : symbol_scale::kTable) {
        INFO("symbol_scale entry missing a cluster: " << entry.symbol);
        CHECK_FALSE(redis_clusters::groupsFor(entry.symbol).empty());
    }
}

TEST_CASE("the cluster map mirrors the C# BuildCluster memberships",
          "[positionClustering]") {
    CHECK(groupsVector("USA500IDXUSD")
          == std::vector<std::string_view>{"US_Index"});
    CHECK(groupsVector("AUDNZD")
          == std::vector<std::string_view>{"Commodity_FX", "Crosses",
                                           "AUD_Pairs"});
    CHECK(groupsVector("EURNOK")
          == std::vector<std::string_view>{"Commodity_FX", "Crosses",
                                           "EUR_Pairs", "Scandi_Pairs"});
    CHECK(groupsVector("USDSEK")
          == std::vector<std::string_view>{"USD_Forex", "Scandi_Pairs"});
    // Symbols the engine does not trade stay in the map on purpose (the C#
    // producer may still cluster them); unknown symbols resolve empty.
    CHECK(groupsVector("GBPAUD")
          == std::vector<std::string_view>{"Crosses", "GBP_Pairs",
                                           "AUD_Pairs"});
    CHECK(groupsVector("NGASCMDUSD")
          == std::vector<std::string_view>{"Energy"});
    CHECK(redis_clusters::groupsFor("DOGEUSD").empty());
    CHECK(redis_clusters::groupsFor("").empty());
}

TEST_CASE("cluster limits and strictness mirror the C# tables",
          "[positionClustering]") {
    CHECK(redis_clusters::clusterLimit("US_Index") == 1);
    CHECK(redis_clusters::clusterLimit("USD_Forex") == 2);
    CHECK(redis_clusters::clusterLimit("Scandi_Pairs") == 1);
    // The C# GroupLimits fallback: an unlisted cluster defaults to 1.
    CHECK(redis_clusters::clusterLimit("NOT_A_CLUSTER") == 1);

    CHECK(redis_clusters::isStrictCluster("US_Index"));
    CHECK(redis_clusters::isStrictCluster("Precious_Metals"));
    CHECK_FALSE(redis_clusters::isStrictCluster("USD_Forex"));
    CHECK_FALSE(redis_clusters::isStrictCluster("Scandi_Pairs"));
    CHECK_FALSE(redis_clusters::isStrictCluster("NOT_A_CLUSTER"));
}

TEST_CASE("evaluateClusterGroup applies the C# checks in order",
          "[positionClustering]") {
    SECTION("empty cluster allows") {
        CHECK(redis_clusters::evaluateClusterGroup(
                  {}, "EURUSD", "OhlcBreakoutStrategy", 2, false)
              == ClusterVerdict::Allowed);
    }

    SECTION("under the limit with no conflicts allows") {
        const std::vector<std::string> members{
            "GBPUSD#RandomStrategy#IGREF-1"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "EURUSD", "OhlcBreakoutStrategy", 2, false)
              == ClusterVerdict::Allowed);
    }

    SECTION("a full cluster blocks, even for an unrelated strategy") {
        const std::vector<std::string> members{
            "GBPUSD#RandomStrategy#IGREF-1",
            "USDJPY#RandomStrategy#IGREF-2"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "EURUSD", "OhlcBreakoutStrategy", 2, false)
              == ClusterVerdict::GroupFull);
    }

    SECTION("capacity outranks stacking (the C# check order)") {
        const std::vector<std::string> members{
            "USA500IDXUSD#OhlcBreakoutStrategy#IGREF-1"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "USA500IDXUSD", "OhlcBreakoutStrategy", 1, true)
              == ClusterVerdict::GroupFull);
    }

    SECTION("the same (symbol, strategy) already open blocks — stacking") {
        const std::vector<std::string> members{
            "EURUSD#OhlcBreakoutStrategy#IGREF-1"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "EURUSD", "OhlcBreakoutStrategy", 2, false)
              == ClusterVerdict::SymbolStrategyStacked);
    }

    SECTION("same strategy, another symbol: blocked when strict") {
        // Every strict cluster ships with limit 1 today, so this branch is
        // shadowed by GroupFull through the real tables — it guards the day
        // a strict cluster's capacity is raised (the C# has the same
        // structure).
        const std::vector<std::string> members{
            "USA30IDXUSD#OhlcBreakoutStrategy#IGREF-1"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "USA500IDXUSD", "OhlcBreakoutStrategy", 2, true)
              == ClusterVerdict::StrategyNotDiverse);
    }

    SECTION("same strategy, another symbol: allowed when not strict") {
        const std::vector<std::string> members{
            "GBPUSD#OhlcBreakoutStrategy#IGREF-1"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "EURUSD", "OhlcBreakoutStrategy", 2, false)
              == ClusterVerdict::Allowed);
    }

    SECTION("malformed members are skipped, not trusted") {
        const std::vector<std::string> members{"garbage-without-hashes"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "EURUSD", "OhlcBreakoutStrategy", 2, true)
              == ClusterVerdict::Allowed);
    }

    SECTION("a two-field member still matches (the C# parts >= 2 tolerance)") {
        const std::vector<std::string> members{"EURUSD#OhlcBreakoutStrategy"};
        CHECK(redis_clusters::evaluateClusterGroup(
                  members, "EURUSD", "OhlcBreakoutStrategy", 2, false)
              == ClusterVerdict::SymbolStrategyStacked);
    }
}

TEST_CASE("clusterMemberString writes the CG# member wire format",
          "[positionClustering]") {
    CHECK(redis_clusters::clusterMemberString(
              {.symbol = "XAUUSD",
               .strategyName = "Fvg",
               .dealReference = "ref-1"})
          == "XAUUSD#Fvg#ref-1");
}

TEST_CASE("groupMembersByCluster fans members out, dedups and drops unmapped "
          "symbols",
          "[positionClustering]") {
    // Injected lookup — the grouping machinery is under test, not the
    // current kClusterTable (same seam discipline as MarketLookup).
    static constexpr std::array<std::string_view, 2> aaaGroups{"C1", "C2"};
    static constexpr std::array<std::string_view, 1> bbbGroups{"C2"};
    const redis_clusters::GroupsLookup lookup =
        [](const std::string_view symbol)
        -> std::span<const std::string_view> {
        if (symbol == "AAA") {
            return aaaGroups;
        }
        if (symbol == "BBB") {
            return bbbGroups;
        }
        return {};
    };

    const std::vector<redis_clusters::ClusterMember> members{
        {.symbol = "AAA", .strategyName = "s1", .dealReference = "d1"},
        {.symbol = "BBB", .strategyName = "s2", .dealReference = "d2"},
        // Exact duplicate — the C# HashSet collapses it.
        {.symbol = "AAA", .strategyName = "s1", .dealReference = "d1"},
        // Unmapped symbol — dropped, like the C# TryGetValue skip.
        {.symbol = "ZZZ", .strategyName = "s3", .dealReference = "d3"},
    };

    const auto clusters = redis_clusters::groupMembersByCluster(members, lookup);
    REQUIRE(clusters.size() == 2);
    REQUIRE(clusters.contains("C1"));
    REQUIRE(clusters.contains("C2"));
    CHECK(clusters.at("C1") == std::vector<std::string>{"AAA#s1#d1"});
    CHECK(clusters.at("C2")
          == std::vector<std::string>{"AAA#s1#d1", "BBB#s2#d2"});
}

TEST_CASE("groupMembersByCluster of nothing is an empty map",
          "[positionClustering]") {
    CHECK(redis_clusters::groupMembersByCluster({}, redis_clusters::groupsFor)
              .empty());
}
