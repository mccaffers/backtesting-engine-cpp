// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace redis_util {
class SyncRedisClient;
}

// Redis-backed broker position store, mirroring the C# engine's
// PositionManager. Three key families, a wire contract shared with the C#
// engine and the external position producer (which repopulates the PO# deals
// from the broker every ~2 minutes):
//
//   PL#<strategyId>  ->  JSON array of open deal ids for that strategy
//   PO#<dealId>      ->  the live deal payload, SHORT TTL: written when the
//                        engine opens a position and refreshed by the external
//                        producer, so a deal the broker never confirms (or has
//                        closed) simply expires and falls out of the PL# list
//                        on the next refreshPositionList
//   PH#<dealId>      ->  closed deal kept 60 days for debugging/history
//
// Same isolation pattern as tradeLocks.hpp: this header is Asio-free so
// module global module fragments can #include it alongside `import std`; the
// connection machinery (redis_util::SyncRedisClient — the deadline-bounded,
// fail-fast pump) lives behind positionManager.cpp.
namespace redis_positions {

// Key builders — free functions so tests can pin the formats (a drifted
// prefix would silently split the position space between the two engines and
// the producer) without a Redis server.
std::string positionListKey(const std::string& strategyId);    // "PL#<id>"
std::string positionKey(const std::string& dealId);            // "PO#<id>"
std::string historyPositionKey(const std::string& dealId);     // "PH#<id>"
// "DealId#<dealReference>#<symbol>" — the C# deal receipt written next to
// every accepted open, kept 60 days for traceability (maps a client deal
// reference back to strategy/deal metadata long after the position closed).
std::string dealReceiptKey(const std::string& dealReference,
                           const std::string& symbol);

// The PL# value is a JSON array of deal-id strings (the C# side writes it
// with JsonSerializer.Serialize(List<string>)). Codec exposed for the same
// reason as the key builders. decode returns nullopt for anything that is
// not a JSON array of strings.
std::string encodePositionList(const std::vector<std::string>& dealIds);
std::optional<std::vector<std::string>> decodePositionList(
    const std::string& json);

// One decoded PO# payload — the shape orderChannel::buildPositionPayload
// writes when the engine opens a deal (and the external producer later
// overwrites from the broker book).
struct PositionRecord {
    std::string dealId;         // empty until the deal confirmed
    std::string dealReference;  // the PO#/PL# book key
    std::string symbol;
    std::string epic;
    std::string direction;      // "BUY" | "SELL"
    double size{};              // broker units (post size-modifier)
    std::int32_t level{};       // scaled INT32 points (see priceData)
    std::int32_t stopLevel{};
    std::int32_t limitLevel{};
    std::string strategyId;
    std::string strategyName;
    std::int64_t openedAtMicros{};
};

// Decode tolerantly — only dealReference, symbol and direction are required
// (a producer-rewritten payload may drop the rest); nullopt for non-objects
// or when a required field is missing/empty.
std::optional<PositionRecord> decodePositionRecord(const std::string& json);

// The inverse: the exact PO# wire shape orderChannel::buildPositionPayload
// writes (same field order, hand-rolled — nlohmann sorts keys alphabetically,
// which would drift the stored shape from the C# JsonSerializer's). Used by
// the position producer to rewrite a record it just updated.
std::string encodePositionRecord(const PositionRecord& record);

// The DealId#<dealReference>#<symbol> receipt payload, as written by
// orderChannel::buildDealReceipt (and the C# engine before it) — only the
// fields the position producer reads back. Decode is tolerant like
// decodePositionRecord: nullopt only for unparseable/non-object JSON; absent
// fields decode as empty strings (the caller substitutes "Unknown").
struct DealReceipt {
    std::string strategyId;
    std::string dealId;
    std::string strategyName;
};
std::optional<DealReceipt> decodeDealReceipt(const std::string& json);

class PositionManager {
public:
    // Connects lazily on first use via the shared Boost.Redis connection
    // (host from $REDIS_HOST in the caller; port 6379 by convention).
    PositionManager(const std::string& host, int port);
    ~PositionManager();
    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    // GET PL#<strategyId>. A missing list key means "no open positions" and
    // returns an empty vector. nullopt means UNKNOWN — Redis unreachable,
    // timed out, or the stored value was not a JSON string array (logged) —
    // so callers gating entries on the count can fail closed.
    std::optional<std::vector<std::string>> getPositionsList(
        const std::string& strategyId);

    // Number of PL# deals whose PO# still exists (getPositionPayloads'
    // survivor rule), NOT the raw list size: nothing in THIS engine prunes
    // PL# when the broker itself closes a deal (stop/limit), so a raw count
    // would hold the strategy at MAX_OPEN_TRADES until the producer next
    // rewrites the list — an expired PO# must stop counting the moment the
    // book sync stops seeing it. 0 when the list key is missing; nullopt
    // when the state is unknown (see getPositionsList).
    std::optional<int> getPositionCount(const std::string& strategyId);

    // PL#<strategyId> -> (dealReference, PO# payload) pairs in list order,
    // READ-ONLY (never rewrites PL# — pruning is refreshPositionList's /
    // the producer's contract). References whose PO# has expired are
    // OMITTED: an expired deal is unconfirmed or closed at the broker (the
    // PO#-TTL doctrine). Empty vector = no open positions; nullopt =
    // UNKNOWN (Redis unreachable or a corrupt list).
    std::optional<std::vector<std::pair<std::string, std::string>>>
    getPositionPayloads(const std::string& strategyId);

    // SET PO#<dealId> payload PX(ttl) — records the deal the moment the
    // engine opens it. The TTL is deliberately SHORT: the external producer
    // refreshes the key every ~2 minutes from the broker's own book, so the
    // default outlives two missed refresh cycles and an unconfirmed deal
    // self-expires instead of counting against the strategy forever.
    bool savePosition(const std::string& dealId, const std::string& payload,
                      std::chrono::milliseconds ttl = std::chrono::minutes{5});

    // Appends dealId to the PL# list (creating it if absent); a dealId
    // already present is left alone. Returns false when the list state is
    // unknown or the write failed.
    bool addPosition(const std::string& strategyId, const std::string& dealId);

    // SET DealId#<dealReference>#<symbol> payload PX(ttl) — the C# deal
    // receipt (60-day default, matching the PH# history retention).
    bool saveDealReceipt(const std::string& dealReference,
                         const std::string& symbol, const std::string& payload,
                         std::chrono::hours ttl = std::chrono::hours{24 * 60});

    // Re-derives the PL# list from the PO# deals that still exist: MGET
    // drops expired deals, survivors are written back in their original
    // order, and a list with no survivors is deleted — the C# refresh loop.
    // Returns false when Redis failed mid-refresh (the list is left as-is).
    bool refreshPositionList(const std::string& strategyId);

    // GET PO#<dealId>, raw. Outer nullopt = Redis failure (state UNKNOWN —
    // the producer must not rebuild a possibly-live record from scratch);
    // inner nullopt = key missing (a genuinely new/expired deal, the fresh-
    // save path).
    std::optional<std::optional<std::string>> getPositionPayload(
        const std::string& dealId);

    // GET PH#<dealId>, raw — the 60-day close-history record. Same contract
    // as getPositionPayload: outer nullopt = Redis failure, inner nullopt =
    // missing. The tracking consumer reads history first (a DELETED deal has
    // usually already been archived) before falling back to the live PO#.
    std::optional<std::optional<std::string>> getHistoryPositionPayload(
        const std::string& dealId);

    // GET DealId#<dealReference>#<symbol>, raw. nullopt for missing AND for
    // failed — the producer coalesces both to strategy "Unknown", exactly
    // like the C# LookUpDealId's null propagation.
    std::optional<std::string> getDealReceipt(const std::string& dealReference,
                                              const std::string& symbol);

    // Every strategyId with a PL# list, via SCAN PL#* (never KEYS), sorted
    // for deterministic logs. The producer refreshes each — the C# iterated
    // its deployed-strategies config, which this engine does not have; the
    // keyspace itself is the equivalent source (and also covers lists whose
    // strategy is no longer deployed). nullopt = Redis failure.
    std::optional<std::vector<std::string>> listStrategyIds();

    // Closes a deal: moves PO#<dealId> to PH#<dealId> with a 60-day TTL
    // (GETDEL + SET rather than RENAME — a RENAME on an already-expired deal
    // raises a Redis error, which would trip the client's circuit breaker for
    // a perfectly normal case) and removes the id from the PL# list.
    bool removePosition(const std::string& strategyId,
                        const std::string& dealId);

private:
    // Missing distinguishes "no PL# key" (a valid empty book) from Failed
    // (unknown state): refresh skips the former and aborts on the latter.
    enum class ListState { Ok, Missing, Failed };
    ListState fetchList(const std::string& strategyId,
                        std::vector<std::string>& dealIds);

    std::unique_ptr<redis_util::SyncRedisClient> client_;
    // Serialises concurrent callers sharing one instance (one connection, one
    // synchronous pump) — same pattern as TradeLocks; prefer one instance per
    // thread (see RedisPositionCounter).
    std::mutex mutex_;
};

}  // namespace redis_positions
