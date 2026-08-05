// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// orderChannel — the decision-to-market request layer, porting the C#
// engine's RequestOpenTrade.Request() and IGMarketCalls.ClosePosition /
// SavePosition / DeletePosition flows around the broker call seams.
//
// OPEN (one OrderRequest in, one placement attempt out):
//   missing market definition -> log, no broker call, signal dropped (the
//       gate's lock simply runs out its TTL — C# semantics; the repeat log
//       is rate-limited by that same TTL)
//   before placing            -> extendLock, so the lock cannot lapse while
//       the order is in flight
//   Accepted                  -> book the deal under IG's echoed
//       dealReference (the dealId does not exist until the deal confirms;
//       the external producer reconciles it later): savePosition (PO#,
//       short TTL) + addPosition (PL# list) + saveDealReceipt (DealId#...,
//       60 days). The lock is left to expire, preserving the throttle.
//   Rejected                  -> releaseLock, the strategy may re-enter early
//   Failed / placeOrder threw -> extendLock for the failure TTL (the C#
//       "trade didn't complete — updating trade lock for 2 minutes")
//
// CLOSE (a booked position in, one close attempt out):
//   blank dealId              -> refused (nothing addressable at the broker)
//   recently closed           -> refused (the C# dealId#CLOSE 5-minute
//       cache: while a close is in flight, repeat close signals for the
//       same deal must not stack further market orders)
//   Ok                        -> removePosition (PO# -> PH# archive + PL#
//       prune) and the deal enters the recently-closed window
//   Gone (no response at all) -> removePosition too — the C# branch: it is
//       missing from IG as far as anyone can tell, and a phantom book entry
//       would haunt the strategy logic indefinitely (the producer restores
//       it from the broker book if it actually still exists)
//   Failed                    -> keep the book entry; the position still
//       exists as far as anyone knows
//
// By the time an open reaches this channel the runner has already run the
// caps and the lock gate (decide -> caps -> gate -> sink) — the gate
// ACQUIRED the lock this channel extends or releases.
//
// Everything side-effectful is injected (market lookup, broker calls, the
// lock/position hooks), so the flow is unit-testable without Redis or a
// broker; brokerOrderSink binds the real implementations.

export module orderChannel;

import std;                // replaces <chrono>, <functional>, <map>, <string>, <utility>
import igMarkets;          // ig::TradeOpenObj/TradeCloseObj, results, seams
import backtestLog;        // backtest_log::logLine
import liveTrace;          // live_trace::emit — live-traces documents
import marketDefinitions;  // live::MarketDefinition, MarketLookup
import orderRequest;       // live::OrderRequest
import trade;              // Direction

export namespace live {

// Everything a close needs from the position book. size is in BROKER units
// (as opened — TRADING_SIZE already multiplied by the market's
// sizeModifier); direction is the OPEN position's engine direction.
struct CloseRequest {
    std::string strategyUuid;
    std::string strategyName;
    std::string symbol;
    Direction direction{Direction::LONG};
    double size{};
    std::string dealId;         // broker deal id — required
    std::string dealReference;  // the PO#/PL# book key
};

// The PO# payload written the moment the broker accepts a deal: the open
// request as JSON under the booked reference. The external position
// producer overwrites it from the broker's own book on its ~2-minute
// refresh, so this only needs to describe the open — exported so tests can
// pin the fields the C# tooling greps for. dealId comes from the confirms
// poll and is empty when the confirm never resolved.
[[nodiscard]] std::string buildPositionPayload(const OrderRequest& request,
                                               const ig::TradeOpenObj& order,
                                               const std::string& dealReference,
                                               const std::string& dealId);

// The DealId#<ref>#<symbol> receipt payload — the C# DealReceipt shape.
// Dated from the decision tick (deterministic under test; within a second
// of the C# DateTime.UtcNow it replaces).
[[nodiscard]] std::string buildDealReceipt(const OrderRequest& request,
                                           const std::string& dealReference,
                                           const std::string& dealId);

class OrderChannel {
public:
    // Lock/position bookkeeping seams. Directions passed to the lock hooks
    // are the engine's "LONG"/"SHORT" — the SAME strings the runner's gate
    // used to acquire (lock keys are LOCK#<uuid>#<LONG|SHORT>); only the
    // broker payloads speak "BUY"/"SELL".
    struct Hooks {
        // The C# PositionClustering.CheckForCluster: TRUE = block the open
        // (cluster at capacity, same-(symbol, strategy) already open, or a
        // strict cluster losing strategy diversity). Consulted after the
        // gate's lock but before anything touches the broker.
        std::function<bool(const std::string& symbol,
                           const std::string& strategyName)>
            clusterBlocked;
        // Called on the Accepted branch only (the open is live at IG):
        // re-arms the symbol's CLUSTER_LOCK cooldowns across the CG#
        // staleness window — the producer's next sync is what makes the new
        // deal visible to clusterBlocked, minutes from now. Optional so
        // tests scripting the other hooks need not wire it.
        std::function<void(const std::string& symbol)> clusterOpened;
        std::function<bool(const std::string& strategyUuid,
                           const std::string& direction,
                           std::chrono::seconds ttl)>
            extendLock;
        std::function<bool(const std::string& strategyUuid,
                           const std::string& direction)>
            releaseLock;
        std::function<bool(const std::string& dealReference,
                           const std::string& payload)>
            savePosition;
        std::function<bool(const std::string& strategyUuid,
                           const std::string& dealReference)>
            addPosition;
        std::function<bool(const std::string& dealReference,
                           const std::string& symbol,
                           const std::string& payload)>
            saveDealReceipt;
        std::function<bool(const std::string& strategyUuid,
                           const std::string& dealReference)>
            removePosition;
    };

    // inFlightTtl restarts the gate's lock just before placing (normally the
    // same TTL the gate acquired with); failureTtl is the C# two-minute
    // brake after a placement that didn't complete; closedTtl is the C#
    // 5-minute dealId#CLOSE window suppressing repeat closes of one deal.
    OrderChannel(MarketLookup lookup, ig::PlaceOrder placeOrder,
                 ig::PlaceClose placeClose, Hooks hooks,
                 std::chrono::seconds inFlightTtl = std::chrono::seconds{30},
                 std::chrono::seconds failureTtl = std::chrono::seconds{120},
                 std::chrono::seconds closedTtl = std::chrono::minutes{5});

    // True only when the broker accepted the open. All outcomes are logged;
    // bookkeeping failures (Redis down while recording an ACCEPTED deal) are
    // logged loudly but still return true — the deal is live regardless, and
    // the external producer rebuilds PL#/PO# from the broker's book within
    // its refresh cadence.
    bool request(const OrderRequest& request);

    // True only when the broker accepted the close. NOT thread-safe with
    // itself (the recently-closed window is instance state) — same
    // one-instance-per-worker-thread discipline as everything else here.
    bool closePosition(const CloseRequest& request);

private:
    MarketLookup lookup_;
    ig::PlaceOrder placeOrder_;
    ig::PlaceClose placeClose_;
    Hooks hooks_;
    std::chrono::seconds inFlightTtl_;
    std::chrono::seconds failureTtl_;
    std::chrono::seconds closedTtl_;
    // dealId -> when its recently-closed suppression window ends. In-memory
    // like the C# TtlCacheService (per-process there, per-worker here — a
    // strategy's deals are only ever touched by its own worker).
    std::map<std::string, std::chrono::steady_clock::time_point> recentCloses_;
};

}  // namespace live

namespace live {

namespace {

std::string_view lockDirection(const Direction direction) {
    return direction == Direction::LONG ? "LONG" : "SHORT";
}

std::string_view brokerDirection(const Direction direction) {
    return direction == Direction::LONG ? "BUY" : "SELL";
}

std::string isoUtcSeconds(const std::chrono::system_clock::time_point tp) {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(tp));
}

}  // namespace

std::string buildPositionPayload(const OrderRequest& request,
                                 const ig::TradeOpenObj& order,
                                 const std::string& dealReference,
                                 const std::string& dealId) {
    // Hand-rolled on purpose: every field is an internal identifier or a
    // number (no user text to escape), matching the flat shape the C# engine
    // stores. openedAt is the decision tick in epoch microseconds.
    return std::format(
        R"({{"dealId":"{}","dealReference":"{}","symbol":"{}","epic":"{}",)"
        R"("direction":"{}","size":{},"level":{},"stopLevel":{},)"
        R"("limitLevel":{},"strategyId":"{}","strategyName":"{}",)"
        R"("openedAt":{}}})",
        dealId, dealReference, request.symbol, order.epic, order.direction,
        order.size, request.level, request.stopLevel, request.limitLevel,
        request.strategyUuid, request.strategyName,
        std::chrono::duration_cast<std::chrono::microseconds>(
            request.timestamp.time_since_epoch())
            .count());
}

std::string buildDealReceipt(const OrderRequest& request,
                             const std::string& dealReference,
                             const std::string& dealId) {
    // The C# DealReceipt: id/sort mirror the key parts.
    return std::format(
        R"({{"id":"DealId#{}","sort":"{}","date":"{}","dealReference":"{}",)"
        R"("strategyId":"{}","dealId":"{}","strategyName":"{}"}})",
        dealReference, request.symbol, isoUtcSeconds(request.timestamp),
        dealReference, request.strategyUuid, dealId, request.strategyName);
}

OrderChannel::OrderChannel(MarketLookup lookup, ig::PlaceOrder placeOrder,
                           ig::PlaceClose placeClose, Hooks hooks,
                           const std::chrono::seconds inFlightTtl,
                           const std::chrono::seconds failureTtl,
                           const std::chrono::seconds closedTtl)
    : lookup_(std::move(lookup)), placeOrder_(std::move(placeOrder)),
      placeClose_(std::move(placeClose)), hooks_(std::move(hooks)),
      inFlightTtl_(inFlightTtl), failureTtl_(failureTtl),
      closedTtl_(closedTtl) {}

bool OrderChannel::request(const OrderRequest& request) {
    using backtest_log::logLine;

    const MarketDefinition* market = lookup_(request.symbol);
    if (market == nullptr) {
        logLine("OrderChannel: {} - missing from marketDefinitions; dropping "
                "{} {} signal",
                request.symbol, request.strategyName,
                lockDirection(request.direction));
        if (live_trace::enabled()) {
            live_trace::emit(
                "orderDropped",
                {.strategyUuid = request.strategyUuid,
                 .strategyName = request.strategyName,
                 .symbol = request.symbol,
                 .dealReference = request.dealReference},
                {{"reason", "missingMarketDefinition"},
                 {"direction", lockDirection(request.direction)}});
        }
        return false;
    }

    const std::string uuid = request.strategyUuid;
    const std::string direction{lockDirection(request.direction)};

    // Portfolio cluster gate (the C# CheckForCluster), after the trade lock
    // and before the broker: correlated-asset capacity, same-symbol
    // stacking and strict-cluster strategy diversity, read from the
    // producer-maintained CG# sets. A block leaves the gate's lock to run
    // out its own TTL — same as the C#, where the lock keeps throttling
    // re-signals while the cluster stays busy.
    if (hooks_.clusterBlocked(request.symbol, request.strategyName)) {
        logLine("OrderChannel: {} - cluster gate blocked {} {} — leaving "
                "the trade lock to expire",
                request.symbol, request.strategyName, direction);
        if (live_trace::enabled()) {
            live_trace::emit("orderBlocked",
                             {.strategyUuid = uuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference},
                             {{"reason", "cluster"},
                              {"direction", direction}});
        }
        return false;
    }

    // Keep the gate's lock alive while the order is in flight. A false here
    // (Redis unreachable) is logged by the lock layer and NOT fatal: the
    // gate acquired the lock moments ago, so its original TTL comfortably
    // covers one placement — same as the C# ExtendLock fire-and-forget.
    hooks_.extendLock(uuid, direction, inFlightTtl_);

    // Always the MINI contract (IGMarketIdentiferMini), and the C#
    // TradeSizeModifier semantics: size scales only when the market sets one.
    ig::TradeOpenObj order{
        .currencyCode = std::string{market->currency},
        .epic = std::string{market->epicMini},
        .direction = std::string{brokerDirection(request.direction)},
        .size = static_cast<double>(request.size) * market->sizeModifier(),
        .stopDistance = request.stopDistancePips,
        .limitDistance = request.limitDistancePips,
        .dealReference = request.dealReference,
    };
    const ig::OrderContext context{
        .strategyUuid = request.strategyUuid,
        .strategyName = request.strategyName,
        .symbol = request.symbol,
        .openDirection = order.direction,
    };

    // A throwing broker call is indistinguishable from a transport failure:
    // the order may or may not have reached IG, so it takes the Failed path
    // (and must not bubble up past the sink into the worker's tick guard).
    ig::OpenResult result;
    try {
        result = placeOrder_(order, context);
    } catch (const std::exception& e) {
        result = ig::OpenResult{.status = ig::OpenStatus::Failed,
                                .reason = e.what()};
    } catch (...) {
        result = ig::OpenResult{.status = ig::OpenStatus::Failed,
                                .reason = "non-std exception from placeOrder"};
    }

    switch (result.status) {
    case ig::OpenStatus::Accepted: {
        // Book under IG's echoed reference; when the echo is missing fall
        // back to the reference WE sent — it names the same deal (the C#
        // TEMP-guid fallback, minus the fresh guid nobody can correlate).
        std::string reference = result.dealReference;
        if (reference.empty()) {
            reference = request.dealReference;
            logLine("OrderChannel: {} - accepted open echoed no "
                    "dealReference; booking under ours ({})",
                    request.symbol, reference);
        }
        logLine("OrderChannel: TRADE OPEN | {} | Strategy: {} | Deal: {} "
                "(dealId={}) | {}",
                request.symbol, request.strategyUuid, reference,
                result.dealId.empty() ? "unconfirmed" : result.dealId,
                order.size);
        // Before the book-keeping: the deal exists at IG from this instant,
        // and every Redis write below can fail without changing that.
        if (hooks_.clusterOpened) {
            hooks_.clusterOpened(request.symbol);
        }
        const bool saved = hooks_.savePosition(
            reference,
            buildPositionPayload(request, order, reference, result.dealId));
        const bool listed = hooks_.addPosition(uuid, reference);
        const bool receipt = hooks_.saveDealReceipt(
            reference, request.symbol,
            buildDealReceipt(request, reference, result.dealId));
        if (!saved || !listed || !receipt) {
            logLine("OrderChannel: {} - deal {} is LIVE but recording it "
                    "failed (PO# saved={}, PL# listed={}, receipt={}); the "
                    "position producer will rebuild from the broker book",
                    request.symbol, reference, saved, listed, receipt);
        }
        if (live_trace::enabled()) {
            // confirmed=false is the never-resolved-confirm path: the POST
            // succeeded but no dealId arrived (see igRequests makeOpen).
            live_trace::emit(
                "orderAccepted",
                {.strategyUuid = uuid,
                 .strategyName = request.strategyName,
                 .symbol = request.symbol,
                 .dealReference = reference,
                 .dealId = result.dealId},
                {{"direction", direction},
                 {"confirmed", !result.dealId.empty()},
                 {"brokerSize", order.size},
                 {"size", static_cast<std::int64_t>(request.size)},
                 {"level", static_cast<std::int64_t>(request.level)},
                 {"stopLevel", static_cast<std::int64_t>(request.stopLevel)},
                 {"limitLevel",
                  static_cast<std::int64_t>(request.limitLevel)},
                 {"epic", order.epic},
                 {"bookkeepingOk", saved && listed && receipt}});
        }
        return true;
    }
    case ig::OpenStatus::Rejected:
        logLine("OrderChannel: {} - broker REJECTED {} ({}): {} — releasing "
                "trade lock for early re-entry",
                request.symbol, order.direction, request.strategyName,
                result.reason);
        hooks_.releaseLock(uuid, direction);
        if (live_trace::enabled()) {
            live_trace::emit("orderRejected",
                             {.strategyUuid = uuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference},
                             {{"direction", direction},
                              {"detail", result.reason}});
        }
        return false;
    case ig::OpenStatus::Failed:
    default:
        logLine("OrderChannel: {} - the trade didn't complete ({}): {} — "
                "updating trade lock for {} seconds",
                request.symbol, request.strategyName, result.reason,
                failureTtl_.count());
        hooks_.extendLock(uuid, direction, failureTtl_);
        if (live_trace::enabled()) {
            live_trace::emit("orderFailed",
                             {.strategyUuid = uuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference},
                             {{"direction", direction},
                              {"detail", result.reason},
                              {"failureTtlSeconds", failureTtl_.count()}});
        }
        return false;
    }
}

bool OrderChannel::closePosition(const CloseRequest& request) {
    using backtest_log::logLine;

    if (request.dealId.empty()) {
        // Nothing addressable at the broker — the C# missing-deal guard.
        logLine("OrderChannel: {} - close refused: missing dealId for {} ({})",
                request.symbol, request.strategyName, request.strategyUuid);
        if (live_trace::enabled()) {
            live_trace::emit("closeRefused",
                             {.strategyUuid = request.strategyUuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference},
                             {{"reason", "missingDealId"}});
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (const auto it = recentCloses_.find(request.dealId);
        it != recentCloses_.end()) {
        if (now < it->second) {
            logLine("OrderChannel: {} - close of {} suppressed (a close "
                    "was accepted within the last {}s)",
                    request.symbol, request.dealId, closedTtl_.count());
            if (live_trace::enabled()) {
                live_trace::emit("closeRefused",
                                 {.strategyUuid = request.strategyUuid,
                                  .strategyName = request.strategyName,
                                  .symbol = request.symbol,
                                  .dealReference = request.dealReference,
                                  .dealId = request.dealId},
                                 {{"reason", "recentlyClosed"}});
            }
            return false;
        }
        recentCloses_.erase(it);
    }

    // Flip the direction: closing a BUY position sells it back, and vice
    // versa.
    const ig::TradeCloseObj close{
        .direction = ig::closingDirection(
            std::string{brokerDirection(request.direction)}),
        .dealId = request.dealId,
        .size = request.size,
    };
    const ig::OrderContext context{
        .strategyUuid = request.strategyUuid,
        .strategyName = request.strategyName,
        .symbol = request.symbol,
        .openDirection = std::string{brokerDirection(request.direction)},
    };
    logLine("OrderChannel: {} - requesting close {} dealId={} size={}",
            request.symbol, close.direction, close.dealId, close.size);

    ig::CloseResult result;
    try {
        result = placeClose_(close, context);
    } catch (const std::exception& e) {
        result = ig::CloseResult{.status = ig::CloseStatus::Failed,
                                 .reason = e.what()};
    } catch (...) {
        result = ig::CloseResult{.status = ig::CloseStatus::Failed,
                                 .reason = "non-std exception from placeClose"};
    }

    switch (result.status) {
    case ig::CloseStatus::Ok: {
        recentCloses_[request.dealId] = now + closedTtl_;
        logLine("OrderChannel: TRADE CLOSE | {} | Strategy: {} | Deal: {} | {}",
                request.symbol, request.strategyUuid, request.dealReference,
                request.size);
        bool bookRemoved = false;
        if (request.dealReference.empty()) {
            // The C# DeletePosition guard: without the book key there is
            // nothing to remove — log and leave the PO# TTL / producer to
            // groom the book.
            logLine("OrderChannel: {} - closed {} but its book reference is "
                    "missing; leaving the book to the producer",
                    request.symbol, request.dealId);
        } else if (hooks_.removePosition(request.strategyUuid,
                                         request.dealReference)) {
            bookRemoved = true;
        } else {
            logLine("OrderChannel: {} - closed {} but removing book entry {} "
                    "failed; the position producer will prune it",
                    request.symbol, request.dealId, request.dealReference);
        }
        if (live_trace::enabled()) {
            live_trace::emit("closeOk",
                             {.strategyUuid = request.strategyUuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference,
                              .dealId = request.dealId},
                             {{"brokerSize", request.size},
                              {"bookRemoved", bookRemoved}});
        }
        return true;
    }
    case ig::CloseStatus::Gone:
        // No response at all: as far as anyone can tell the position is
        // missing from IG — drop the book entry so a phantom cannot pin the
        // strategy's open-trade count forever (C# null-response branch). If
        // it DOES still exist, the producer restores it from the broker
        // book on its next refresh.
        logLine("OrderChannel: {} - close of {} got no response ({}); "
                "treating as missing from IG and removing book entry",
                request.symbol, request.dealId, result.reason);
        if (!request.dealReference.empty()) {
            hooks_.removePosition(request.strategyUuid, request.dealReference);
        }
        if (live_trace::enabled()) {
            live_trace::emit("closeGone",
                             {.strategyUuid = request.strategyUuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference,
                              .dealId = request.dealId},
                             {{"detail", result.reason}});
        }
        return false;
    case ig::CloseStatus::Failed:
    default:
        logLine("OrderChannel: {} - close of {} didn't complete ({}); "
                "keeping the book entry",
                request.symbol, request.dealId, result.reason);
        if (live_trace::enabled()) {
            live_trace::emit("closeFailed",
                             {.strategyUuid = request.strategyUuid,
                              .strategyName = request.strategyName,
                              .symbol = request.symbol,
                              .dealReference = request.dealReference,
                              .dealId = request.dealId},
                             {{"detail", result.reason}});
        }
        return false;
    }
}

}  // namespace live
