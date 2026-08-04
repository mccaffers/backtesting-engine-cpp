// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// positionSync — one cycle of the IG position producer, the C++ port of the
// C# igmarkets_positions cron (vortex igmarkets_positions/IGMarketCalls.cs +
// Program.cs). Each cycle mirrors the broker's own /positions book into the
// Redis position store the rest of the ecosystem reads:
//
//   GET /positions  ->  per matched deal, save/refresh PO#<dealRef> (10-min
//   update TTL, 30-min fresh, weekend hold on Friday nights) and ensure it
//   is in PL#<strategyId>  ->  rebuild the CG# cluster sets  ->  refresh
//   every PL# list (prune deals whose PO# expired)  ->  report each stage
//   to the live-function-logs Elastic index, like the C# LiveFunctionReport.
//
// The producer doctrine: PO# records carry a deliberately SHORT TTL, so this
// sync re-stamping them every minute is what keeps a live deal alive in
// Redis — a deal the broker no longer reports simply expires and falls out
// of the PL# lists on the refresh pass. The pure helpers (TTL rule, date
// parse, JSON decode, record build/update) are exported for the unit tests;
// everything Redis/HTTP flows through the shared PositionManager /
// PositionClustering / ig_rest seams.
//
// The GMF only #includes Asio-free headers (the Redis connection machinery
// stays behind positionManager.cpp / positionClustering.cpp), so it is safe
// to `import std` here; nlohmann in a module GMF follows liveWinners.

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"
#include "shared/ig/igRestClient.hpp"
#include "shared/redis/positionClustering.hpp"
#include "shared/redis/positionManager.hpp"

export module positionSync;

import std;
import backtestLog;         // backtest_log::logLine
import marketDefinitions;   // live::findMarketByEpicMini — the tradable-epic gate
import symbolScale;         // symbol_scale::getPriceScale — decimal -> INT32 points

export namespace positions {

// Same std::function instantiation as ig::AuthProvider (igRequests), spelled
// locally so this module doesn't have to import the whole order machinery —
// the command passes ig::dynamoAuthProvider(...) straight in.
using AuthProvider = std::function<std::optional<ig_rest::Auth>()>;

// One entry of the /positions response (the C# AccountPositions model): the
// nested market/position objects flattened to just the fields the producer
// consumes. IG's nullable doubles (stopLevel/limitLevel) decode as 0,
// matching the C# `?? 0`.
struct IgPosition {
    std::string epic;           // market.epic — matched against epicMini
    std::string dealReference;  // the PO#/PL# book key; skipped when empty
    std::string dealId;
    std::string direction;      // "BUY" | "SELL"
    std::string createdDate;    // IG's "yyyy/MM/dd HH:mm:ss:fff"
    double dealSize{};
    double openLevel{};
    double stopLevel{};   // null/absent -> 0
    double limitLevel{};  // null/absent -> 0
};

// Decode a /positions body. nullopt = undecodable (not an object, or a
// positions value that is neither array nor null) — the C# JsonException
// path, reported FAILED-DESERIALIZE. A decodable body with positions
// null/absent is an EMPTY book, not an error (the C# SavePositions early
// return). Missing market/position sub-objects are tolerated per entry
// (fields stay empty/zero), like the C# null-conditional chains.
[[nodiscard]] std::optional<std::vector<IgPosition>> decodeAccountPositions(
    const std::string& json);

// IG's createdDate ("yyyy/MM/dd HH:mm:ss:fff", UTC — the C# AssumeUniversal)
// -> epoch microseconds. Hand-parsed with from_chars + sys_days: libc++ has
// no std::chrono::parse and gmtime_r/timegm are not in `import std`. Any
// malformed input returns `fallbackMicros` (the caller passes "now") — the
// C# ParseExact threw and killed the cron; in-process the record is simply
// stamped with the sync time instead.
[[nodiscard]] std::int64_t parseCreatedDateMicros(std::string_view createdDate,
                                                  std::int64_t fallbackMicros);

// The update-path TTL rule, pure over the passed instant: 10 minutes, EXCEPT
// Friday 21:55:00–21:59:59 UTC where it becomes 2 days + 2 hours — IG closes
// the week at 21:00 Friday less the CFD after-hours, and a position still
// open then must survive Redis until Sunday-night trading resumes (the C#
// weekend hold, ported verbatim).
[[nodiscard]] std::chrono::milliseconds updateTtl(std::chrono::sys_seconds nowUtc);

// Fresh saves get a longer leash than updates: a brand-new deal's receipt /
// book entries may lag a cycle or two (the C# Save's 30 minutes).
inline constexpr std::chrono::minutes kFreshPositionTtl{30};

// The C# Save(): build a PO# record from the broker's own numbers. Prices
// arrive as decimals and are stored as scaled INT32 points via `priceScale`
// (llround = the C# MidpointRounding.AwayFromZero); a priceScale <= 0
// (symbol_scale::kUnknown) writes level 0, and the caller logs the warning.
// The scale is injected so tests exercise the arithmetic without depending
// on the current symbolScale table. openedAt comes from createdDate with
// `nowMicros` as the malformed-date fallback.
[[nodiscard]] redis_positions::PositionRecord makeFreshRecord(
    const IgPosition& position, std::string_view symbol,
    std::string_view strategyId, std::string_view strategyName, int priceScale,
    std::int64_t nowMicros);

// The C# Update() mutation on an existing PO# record: a missing strategy
// attribution ("" or "Unknown") is filled from the deal receipt, and the
// dealId is refreshed when IG reports one (the C# null-coalesce, hardened
// against an empty string clobbering a known id). Everything else — levels,
// size, openedAt — keeps the stored engine-side values.
void applyBrokerUpdate(redis_positions::PositionRecord& record,
                       const IgPosition& position, std::string_view strategyId);

// One producer instance: owns its Redis connections (one PositionManager,
// one PositionClustering) and the IG session provider. Single-threaded by
// design — the command's minute loop is the only caller, so the members'
// internal mutexes are uncontended.
class Sync {
public:
    struct Config {
        std::string redisHost;
        int redisPort;
        std::string tradingEnv;  // names the Auth#<env> session, log-only here
    };

    Sync(Config config, AuthProvider auth)
        : config_(std::move(config)), auth_(std::move(auth)),
          store_(config_.redisHost, config_.redisPort),
          clusters_(config_.redisHost, config_.redisPort) {}

    Sync(const Sync&) = delete;
    Sync& operator=(const Sync&) = delete;

    // One full cron cycle, in the C# order: fetch -> save/update each
    // matched position -> cluster rebuild -> Success report -> PL# refresh.
    // Any fetch-stage failure (no session, HTTP failure, undecodable body)
    // logs, files a FAILED-* report and returns without touching Redis —
    // the C# threw there, and the refresh pass never ran on a failed fetch.
    // Never throws; a failed cycle is retried by the next minute's.
    void syncOnce();

private:
    void applyPosition(const IgPosition& position,
                       std::vector<redis_clusters::ClusterMember>& members);

    Config config_;
    AuthProvider auth_;
    redis_positions::PositionManager store_;
    redis_clusters::PositionClustering clusters_;

    // Per-cycle counters for the summary line, reset each syncOnce().
    std::size_t matched_{};
    std::size_t fresh_{};
    std::size_t updated_{};
    std::size_t skippedUnknown_{};
};

}  // namespace positions

namespace positions {

namespace {

// The C# shared.Elastic.LiveFunctionReport: {function, action, status,
// details, date} into live-function-logs, queued for the publisher's
// background flusher so an Elastic outage never stalls the sync loop.
void reportFunction(const std::string& function, const std::string& action,
                    const std::string& status, std::string details) {
    nlohmann::json doc{
        {"function", function},        {"action", action},
        {"status", status},            {"details", std::move(details)},
        {"date", elastic::nowIsoUtc()},
    };
    elastic::enqueueDocument("live-function-logs", doc.dump());
}

void readString(const nlohmann::json& object, const char* key,
                std::string& out) {
    if (const auto it = object.find(key);
        it != object.end() && it->is_string()) {
        out = it->get<std::string>();
    }
}

void readNumber(const nlohmann::json& object, const char* key, double& out) {
    // null (IG's absent stop/limit) and missing both leave the 0 default.
    if (const auto it = object.find(key);
        it != object.end() && it->is_number()) {
        out = it->get<double>();
    }
}

std::int64_t nowEpochMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::optional<std::vector<IgPosition>> decodeAccountPositions(
    const std::string& json) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    const auto positionsIt = parsed.find("positions");
    if (positionsIt == parsed.end() || positionsIt->is_null()) {
        return std::vector<IgPosition>{};  // an empty book, not an error
    }
    if (!positionsIt->is_array()) {
        return std::nullopt;
    }
    std::vector<IgPosition> book;
    book.reserve(positionsIt->size());
    for (const auto& entry : *positionsIt) {
        if (!entry.is_object()) {
            return std::nullopt;  // the C# deserializer would throw here
        }
        IgPosition position;
        if (const auto market = entry.find("market");
            market != entry.end() && market->is_object()) {
            readString(*market, "epic", position.epic);
        }
        if (const auto detail = entry.find("position");
            detail != entry.end() && detail->is_object()) {
            readString(*detail, "dealReference", position.dealReference);
            readString(*detail, "dealId", position.dealId);
            readString(*detail, "direction", position.direction);
            readString(*detail, "createdDate", position.createdDate);
            readNumber(*detail, "dealSize", position.dealSize);
            readNumber(*detail, "openLevel", position.openLevel);
            readNumber(*detail, "stopLevel", position.stopLevel);
            readNumber(*detail, "limitLevel", position.limitLevel);
        }
        book.push_back(std::move(position));
    }
    return book;
}

std::int64_t parseCreatedDateMicros(const std::string_view createdDate,
                                    const std::int64_t fallbackMicros) {
    // "yyyy/MM/dd HH:mm:ss:fff" — fixed width, exact separators (the C#
    // ParseExact contract; anything else falls back).
    if (createdDate.size() != 23 || createdDate[4] != '/'
        || createdDate[7] != '/' || createdDate[10] != ' '
        || createdDate[13] != ':' || createdDate[16] != ':'
        || createdDate[19] != ':') {
        return fallbackMicros;
    }
    const auto readInt = [createdDate](const std::size_t pos,
                                       const std::size_t len, int& out) {
        const char* first = createdDate.data() + pos;
        const auto [ptr, ec] = std::from_chars(first, first + len, out);
        return ec == std::errc{} && ptr == first + len;
    };
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millis = 0;
    if (!readInt(0, 4, year) || !readInt(5, 2, month) || !readInt(8, 2, day)
        || !readInt(11, 2, hour) || !readInt(14, 2, minute)
        || !readInt(17, 2, second) || !readInt(20, 3, millis)) {
        return fallbackMicros;
    }
    const std::chrono::year_month_day date{
        std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
        return fallbackMicros;
    }
    const auto instant = std::chrono::sys_days{date} + std::chrono::hours{hour}
                         + std::chrono::minutes{minute}
                         + std::chrono::seconds{second}
                         + std::chrono::milliseconds{millis};
    return std::chrono::duration_cast<std::chrono::microseconds>(
               instant.time_since_epoch())
        .count();
}

std::chrono::milliseconds updateTtl(const std::chrono::sys_seconds nowUtc) {
    const auto day = std::chrono::floor<std::chrono::days>(nowUtc);
    const std::chrono::weekday weekday{day};
    const std::chrono::hh_mm_ss timeOfDay{nowUtc - day};
    if (weekday == std::chrono::Friday
        && timeOfDay.hours() == std::chrono::hours{21}
        && timeOfDay.minutes() >= std::chrono::minutes{55}) {
        return std::chrono::days{2} + std::chrono::hours{2};
    }
    return std::chrono::minutes{10};
}

redis_positions::PositionRecord makeFreshRecord(
    const IgPosition& position, const std::string_view symbol,
    const std::string_view strategyId, const std::string_view strategyName,
    const int priceScale, const std::int64_t nowMicros) {
    const auto scalePrice = [priceScale](const double level) -> std::int32_t {
        if (priceScale <= 0) {
            return 0;  // no scale — level 0, as the C# wrote (caller warns)
        }
        // llround = the C# Math.Round(..., MidpointRounding.AwayFromZero).
        return static_cast<std::int32_t>(std::llround(level * priceScale));
    };
    redis_positions::PositionRecord record;
    record.dealId = position.dealId;
    record.dealReference = position.dealReference;
    record.symbol = std::string{symbol};
    record.epic = position.epic;
    record.direction = position.direction;
    record.size = position.dealSize;
    record.level = scalePrice(position.openLevel);
    record.stopLevel = scalePrice(position.stopLevel);
    record.limitLevel = scalePrice(position.limitLevel);
    record.strategyId = std::string{strategyId};
    record.strategyName = std::string{strategyName};
    record.openedAtMicros =
        parseCreatedDateMicros(position.createdDate, nowMicros);
    return record;
}

void applyBrokerUpdate(redis_positions::PositionRecord& record,
                       const IgPosition& position,
                       const std::string_view strategyId) {
    if (record.strategyId.empty() || record.strategyId == "Unknown") {
        record.strategyId = std::string{strategyId};
    }
    if (!position.dealId.empty()) {
        record.dealId = position.dealId;
    }
}

void Sync::applyPosition(const IgPosition& position,
                         std::vector<redis_clusters::ClusterMember>& members) {
    using backtest_log::logLine;

    const live::MarketDefinition* market =
        live::findMarketByEpicMini(position.epic);
    if (market == nullptr || position.dealReference.empty()) {
        return;  // not an epic this engine trades — the C# match skip
    }
    ++matched_;
    const std::string symbol{market->symbol};

    // Strategy attribution from the deal receipt the order channel wrote
    // when it opened the deal; a deal opened outside the engine (or whose
    // receipt expired) attributes to "Unknown", like the C# coalesce.
    std::string strategyId = "Unknown";
    std::string strategyName = "Unknown";
    if (const auto receiptJson =
            store_.getDealReceipt(position.dealReference, symbol)) {
        if (const auto receipt =
                redis_positions::decodeDealReceipt(*receiptJson)) {
            if (!receipt->strategyId.empty()) {
                strategyId = receipt->strategyId;
            }
            if (!receipt->strategyName.empty()) {
                strategyName = receipt->strategyName;
            }
        }
    }

    // Collected for the cluster rebuild BEFORE the store round trips: the
    // deal is open at the broker, so it occupies its clusters whatever
    // happens to its PO# record below — omitting it would let the entry
    // gate under-count a cluster for the sets' 5-minute TTL. (The C# never
    // faced this: its Redis failures killed the whole cron before
    // SyncAllClusters ran.)
    members.push_back(redis_clusters::ClusterMember{
        .symbol = symbol,
        .strategyName = strategyName,
        .dealReference = position.dealReference});

    const auto payload = store_.getPositionPayload(position.dealReference);
    if (!payload) {
        // Redis state UNKNOWN: rebuilding from scratch could stamp a live,
        // fully-attributed record with an Unknown one — sit this deal out
        // and let the next cycle retry.
        ++skippedUnknown_;
        return;
    }

    std::optional<redis_positions::PositionRecord> record;
    if (payload->has_value()) {
        // Undecodable/incomplete payloads fall through to the fresh-save
        // rebuild, the same rule as the C# DoesThisPositionAlreadyExist.
        record = redis_positions::decodePositionRecord(**payload);
    }

    if (record) {
        applyBrokerUpdate(*record, position, strategyId);
        const auto now = std::chrono::floor<std::chrono::seconds>(
            std::chrono::system_clock::now());
        store_.savePosition(position.dealReference,
                            redis_positions::encodePositionRecord(*record),
                            updateTtl(now));
        store_.addPosition(record->strategyId, record->dealReference);
        ++updated_;
    } else {
        const int priceScale = symbol_scale::getPriceScale(symbol);
        if (priceScale <= 0) {
            logLine("PositionSync: WARNING — no price scale for {}, writing "
                    "level 0",
                    symbol);
        }
        const redis_positions::PositionRecord freshRecord =
            makeFreshRecord(position, symbol, strategyId, strategyName,
                            priceScale, nowEpochMicros());
        const std::string payloadJson =
            redis_positions::encodePositionRecord(freshRecord);
        store_.savePosition(position.dealReference, payloadJson,
                            kFreshPositionTtl);
        store_.addPosition(freshRecord.strategyId, freshRecord.dealReference);
        logLine("PositionSync: new position found! {}", payloadJson);
        reportFunction("PositionRequest", "Position Update", "New",
                       payloadJson);
        ++fresh_;
    }
}

void Sync::syncOnce() {
    using backtest_log::logLine;
    matched_ = fresh_ = updated_ = skippedUnknown_ = 0;

    // 1. Session + fetch. Every failure here files the same FAILED-* report
    // the C# did and abandons the cycle — refresh must never run against a
    // book we could not read (an unreadable book is not an empty one).
    const std::optional<ig_rest::Auth> auth = auth_();
    if (!auth) {
        logLine("PositionSync: no IG session (Auth#{}) — skipping this cycle",
                config_.tradingEnv);
        reportFunction("IG-Account", "PositionRequest", "FAILED-REQUEST",
                       nlohmann::json{{"responseWasNull", true},
                                      {"reason", "no IG session credentials"}}
                           .dump());
        return;
    }
    const std::optional<ig_rest::HttpResponse> response =
        ig_rest::execute(*auth, "/positions", "GET", "");
    if (!response || response->status != 200) {
        logLine("PositionSync: /positions request failed (status={}) — "
                "skipping this cycle",
                response ? response->status : 0);
        reportFunction(
            "IG-Account", "PositionRequest", "FAILED-REQUEST",
            nlohmann::json{{"statusCode", response ? response->status : 500},
                           {"responseWasNull", !response.has_value()},
                           {"content",
                            response ? response->body : "No response content"}}
                .dump());
        return;
    }

    // 2. Decode.
    const auto book = decodeAccountPositions(response->body);
    if (!book) {
        logLine("PositionSync: /positions body would not decode — skipping "
                "this cycle");
        reportFunction("IG-Account", "PositionRequest", "FAILED-DESERIALIZE",
                       nlohmann::json{{"statusCode", response->status},
                                      {"content", response->body}}
                           .dump());
        return;
    }

    // 3. Save/update each matched deal, collecting the cluster members.
    std::vector<redis_clusters::ClusterMember> members;
    members.reserve(book->size());
    for (const IgPosition& position : *book) {
        applyPosition(position, members);
    }

    // 4. Rebuild the CG# sets (non-empty clusters only — see syncAllClusters).
    const bool clustersSynced = clusters_.syncAllClusters(members);

    // 5. The Success report carries the raw broker book, the ground-truth
    // snapshot the C# stored (re-serialised there, verbatim here).
    reportFunction("IG-Account", "PositionRequest", "Success", response->body);

    // 6. Refresh every PL# list so deals whose PO# expired fall out. The C#
    // walked its deployed-strategies config; the keyspace is this engine's
    // equivalent (and also prunes lists for strategies no longer deployed).
    std::size_t refreshed = 0;
    bool scanOk = false;
    if (const auto strategyIds = store_.listStrategyIds()) {
        scanOk = true;
        for (const std::string& strategyId : *strategyIds) {
            if (store_.refreshPositionList(strategyId)) {
                ++refreshed;
            }
        }
    } else {
        logLine("PositionSync: PL#* scan failed — skipping the refresh pass "
                "this cycle");
    }

    logLine("PositionSync: cycle complete (positions={} matched={} fresh={} "
            "updated={} skippedUnknown={} clusterMembers={} clustersSynced={} "
            "listsRefreshed={}{})",
            book->size(), matched_, fresh_, updated_, skippedUnknown_,
            members.size(), clustersSynced, refreshed,
            scanOk ? "" : " REFRESH-SKIPPED");
}

}  // namespace positions
