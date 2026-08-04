// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// igRequests — the C# engine's IGMarketRequests, split into its two halves:
//
//   IGMarketRequests  the guarded request path every broker call funnels
//                     through: duplicate-deal suppression (API#<dealKey>,
//                     30s) -> shared rate budget (REQ#<minute>, 30/min,
//                     shared with the C# engine via Redis) -> record both ->
//                     session credentials -> the retried HTTPS exchange
//                     (shared/ig/igRestClient). OPENS fail closed: an
//                     unknown counter sends nothing — a skipped entry is
//                     recoverable, a duplicate or unaccountable one is not.
//                     CLOSES are risk-reducing and fail OPEN on gate
//                     uncertainty (and bypass the soft budget): a skipped
//                     close leaves live exposure on the book, which is the
//                     one outcome worse than an extra request — a doubled
//                     close is rejected safely by IG. Only a definite
//                     duplicate marker or missing session refuses a close.
//
//   IGMarketCalls     the open/close calls over that path. The mapping
//                     cores (makeOpen/makeClose over an injected RequestFn)
//                     are pure response interpretation, unit-tested without
//                     Redis or a network; makeLiveOpen/makeLiveClose bind
//                     them to a per-worker-thread IGMarketRequests (same
//                     thread_local pattern as RedisTradeGate — the Redis
//                     budget is shared server-side, so per-thread instances
//                     don't weaken it) and add the live-trades Elasticsearch
//                     audit line on success.
//
// The GMF includes are all Asio-free headers (curl and Boost stay behind
// the .cpp implementations).

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"
#include "shared/aws/dynamoAuth.hpp"
#include "shared/ig/igRestClient.hpp"
#include "shared/redis/apiRequestGate.hpp"

export module igRequests;

import std;      // replaces <chrono>, <functional>, <memory>, <optional>, <string>
import igMarkets;  // ig::TradeOpenObj/TradeCloseObj, OpenResult, CloseResult, codecs
import backtestLog;  // backtest_log::logLine
import liveTrace;    // live_trace::emit/tradingEnv — live-traces documents

export namespace ig {

// Session credential source. Injectable for tests; live uses the DynamoDB
// store the external login service refreshes (CST/X-SECURITY-TOKEN expire).
using AuthProvider = std::function<std::optional<ig_rest::Auth>()>;

// The C# Auth.PullWithRetry: MarketDataLive item "Auth#<environment>"
// ("live" / "demo"), pulled FRESH per broker request so a token the login
// service just rotated is picked up immediately. AWS credentials/region
// come from the process environment (SDK default chain).
AuthProvider dynamoAuthProvider(std::string environment);

// Per-request gate/transport configuration, set by the market call that
// owns the request's semantics (open vs close vs confirm poll).
struct RequestOptions {
    // Duplicate-suppression key (API#<dealKey>, 30s). "" = no dedup marker
    // (the confirms GET). Opens key on strategyUuid+direction; closes key on
    // "close#<dealId>" — DISTINCT namespaces, so an open's marker can never
    // refuse the close that follows it.
    std::string dealKey;
    // True for closes: gate uncertainty (Redis unreachable) and the soft
    // budget must not refuse a risk-reducing order. Opens stay fail-closed.
    bool riskReducing = false;
    // Transport retries inside ig_rest::execute. 0 for the NON-IDEMPOTENT
    // open POST (a blind re-send after a lost ACK can double a position —
    // makeOpen resolves ambiguity through /confirms instead).
    int transportRetries = 2;
};

// What actually happened to a guarded request. The old conflation of
// "refused by the gate" with "sent but transport failed" into one nullopt
// is exactly what let a refused close masquerade as a missing position.
enum class RequestFate {
    Refused,          // never sent: gate refusal or no session credentials
    TransportFailed,  // sent (or mid-send): no HTTP exchange completed
    Responded,        // an HTTP response arrived — response is engaged
};

struct RequestOutcome {
    RequestFate fate{RequestFate::Refused};
    std::optional<ig_rest::HttpResponse> response;  // Responded only
    std::string detail;  // refusal reason / transport note for logs & traces
};

// The seam between the market calls and the guarded request path — the C#
// IGMarketRequests.Request signature, carrying the outcome's fate.
using RequestFn = std::function<RequestOutcome(
    const std::string& path, const std::string& method,
    const std::string& jsonBody, const ig_rest::Headers& extraHeaders,
    const RequestOptions& options)>;

// Why the gate refused (nullopt from gatePolicy = proceed). Distinct values
// keep the live-trace `reason` fields diagnosable.
enum class GateRefusal {
    DuplicateDeal,     // marker definitely present
    DuplicateUnknown,  // Redis unreachable, fail-closed (opens only)
    BudgetUnknown,     // Redis unreachable, fail-closed (opens only)
    RateLimited,       // budget known and exhausted (opens only)
};

// The pure gate-policy core, unit-tested without Redis (same doctrine as
// the makeOpen/makeClose mapping cores). `duplicate` nullopt = unknown
// (Redis unreachable); `requestsMade` nullopt likewise. Risk-reducing
// requests proceed on any uncertainty and past the soft budget — only a
// DEFINITE duplicate refuses them.
std::optional<GateRefusal> gatePolicy(std::optional<bool> duplicate,
                                      std::optional<int> requestsMade,
                                      bool riskReducing);

class IGMarketRequests {
public:
    // The C# MAX_REQUESTS_PER_MINUTE — a soft brake shared with the other
    // engine through Redis; IG enforces the hard one.
    static constexpr int kMaxRequestsPerMinute = 30;

    IGMarketRequests(const std::string& redisHost, int redisPort,
                     AuthProvider auth);

    // The guarded request path (see the header comment for the gate order).
    RequestOutcome request(const std::string& path, const std::string& method,
                           const std::string& jsonBody,
                           const ig_rest::Headers& extraHeaders,
                           const RequestOptions& options);

private:
    redis_api::ApiRequestGate gate_;
    AuthProvider auth_;
};

class IGMarketCalls {
public:
    // Mapping cores — pure response interpretation over the injected
    // request path. Open: POST /positions/otc; HTTP 200 with a parseable,
    // error-free body is provisionally Accepted (IG's dealReference echo),
    // then the confirm poll (GET /confirms/{dealReference}, Version 1)
    // resolves the deal's fate: ACCEPTED fills the broker dealId, REJECTED
    // maps to OpenStatus::Rejected (the channel's early-lock-release path).
    // A confirm that never resolves keeps Accepted with an EMPTY dealId:
    // the POST succeeded so IG has the order — Rejected here would release
    // the lock (re-entry -> possible doubled exposure) and skip booking a
    // possibly-live deal, while an empty dealId merely fails closed at
    // close time. Anything murky before the echo (non-2xx, unparseable,
    // errorCode on 200) stays Failed — booking a phantom deal is worse
    // than a spurious 2-minute brake.
    // confirmAttempts/confirmDelay bound the poll (tests pass 0ms; note
    // each attempt spends the shared REQ# minute budget, so an accepted
    // open costs up to 1 + confirmAttempts of the 30/min).
    static PlaceOrder makeOpen(
        RequestFn request, int confirmAttempts = 3,
        std::chrono::milliseconds confirmDelay = std::chrono::milliseconds{300});

    // Close: POST /positions/otc with "_method: DELETE" (Version 1). HTTP
    // 200 + parseable body -> Ok; NO response from the request layer ->
    // Gone (the C# "missing from IG" branch — the caller deletes the book
    // entry); anything else -> Failed (the position may still exist).
    static PlaceClose makeClose(RequestFn request);

    // Live bindings: one IGMarketRequests per calling worker thread, plus
    // the live-trades Elasticsearch audit document on success (C# writes
    // one on open; close is audited here too for a symmetric trail).
    static PlaceOrder makeLiveOpen(std::string redisHost, int redisPort,
                                   AuthProvider auth);
    static PlaceClose makeLiveClose(std::string redisHost, int redisPort,
                                    AuthProvider auth);
};

}  // namespace ig

namespace ig {

namespace {

int currentMinuteOfHour() {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::minutes>(sinceEpoch).count()
        % 60);
}

// One IGMarketRequests per calling thread, lazily built — shared by the
// open and close bindings on that thread (same Redis budget either way).
RequestFn threadLocalRequestFn(std::string redisHost, const int redisPort,
                               AuthProvider auth) {
    return [redisHost = std::move(redisHost), redisPort,
            auth = std::move(auth)](
               const std::string& path, const std::string& method,
               const std::string& jsonBody,
               const ig_rest::Headers& extraHeaders,
               const RequestOptions& options) {
        thread_local std::unique_ptr<IGMarketRequests> requests;
        if (!requests) {
            requests = std::make_unique<IGMarketRequests>(redisHost, redisPort,
                                                          auth);
        }
        return requests->request(path, method, jsonBody, extraHeaders,
                                 options);
    };
}

// Polls GET /confirms/{confirmReference} until the deal's fate resolves.
// ACCEPTED maps to Accepted (reporting `reportReference` — the happy path
// preserves IG's echo, the ambiguity path reports the reference WE sent);
// REJECTED maps to Rejected. nullopt = the confirm never resolved within
// `attempts` — the CALLER decides what unresolved means (Accepted-with-empty-
// dealId after a clean 200, Failed after a transport-ambiguous POST).
std::optional<OpenResult> pollConfirm(const RequestFn& request,
                                      const std::string& confirmReference,
                                      const std::string& reportReference,
                                      const int attempts,
                                      const std::chrono::milliseconds delay) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0 && delay.count() > 0) {
            // The confirm is usually ready immediately; the delay only
            // paces the retries while it propagates.
            std::this_thread::sleep_for(delay);
        }
        // EMPTY dealKey on purpose: the open POST just recorded its own
        // marker for 30s — reusing it would refuse this very confirm as a
        // duplicate. Explicit Version 1 (confirms is a v1 endpoint; ig_rest
        // lets a caller-supplied Version replace the versionFor default).
        const RequestOutcome confirm =
            request("/confirms/" + confirmReference, "GET", "",
                    {{"Version", "1"}}, RequestOptions{});
        if (confirm.fate != RequestFate::Responded
            || confirm.response->status != 200) {
            continue;  // 404 while it propagates, gate refusal, 5xx
        }
        const auto confirmation =
            parseDealConfirmation(confirm.response->body);
        if (!confirmation) {
            continue;
        }
        if (confirmation->dealStatus == "REJECTED") {
            return OpenResult{.status = OpenStatus::Rejected,
                              .dealReference = confirmReference,
                              .reason = confirmation->reason.empty()
                                            ? "REJECTED"
                                            : confirmation->reason};
        }
        if (confirmation->dealStatus == "ACCEPTED") {
            return OpenResult{.status = OpenStatus::Accepted,
                              .dealReference = reportReference,
                              .dealId = confirmation->dealId};
        }
        // Any other status = still pending — keep polling.
    }
    return std::nullopt;
}

void auditTrade(const std::string& action, const OrderContext& context,
                const std::string& dealReference,
                const std::string& dealId = "") {
    // The C# ElasticTradeLogs document shape, index "live-trades". Queued for
    // the publisher's background flusher, so an Elastic outage can never stall
    // an order worker mid-retry; delivery (with retry + dead-letter) happens
    // in periodic _bulk batches. Disable via ELASTIC_ENABLED=0.
    nlohmann::json doc{
        {"date", elastic::nowIsoUtc()},
        {"env", std::string(live_trace::tradingEnv())},
        {"symbol", context.symbol},
        {"action", action},
        {"strategy", context.strategyUuid},
        {"dealReference", dealReference},
    };
    if (!dealId.empty()) {
        doc["dealId"] = dealId;
    }
    elastic::enqueueDocument("live-trades", doc.dump());
}

}  // namespace

AuthProvider dynamoAuthProvider(std::string environment) {
    return [environment = std::move(environment)] {
        return aws_auth::pullAuthWithRetry(environment);
    };
}

IGMarketRequests::IGMarketRequests(const std::string& redisHost,
                                   const int redisPort, AuthProvider auth)
    : gate_(redisHost, redisPort), auth_(std::move(auth)) {}

std::optional<GateRefusal> gatePolicy(const std::optional<bool> duplicate,
                                      const std::optional<int> requestsMade,
                                      const bool riskReducing) {
    // A DEFINITE duplicate refuses both classes: for a close this is pacing
    // (its own close#<dealId> marker), and a refused close now maps to
    // Failed — the book entry survives and the sync loop retries after the
    // 30s window.
    if (duplicate.has_value() && *duplicate) {
        return GateRefusal::DuplicateDeal;
    }
    if (riskReducing) {
        // Risk-reducing (close): uncertainty and the soft budget never
        // refuse — an unsent close leaves live exposure, which is worse
        // than any doubled or over-budget request IG rejects safely.
        return std::nullopt;
    }
    if (!duplicate.has_value()) {
        return GateRefusal::DuplicateUnknown;
    }
    if (!requestsMade.has_value()) {
        return GateRefusal::BudgetUnknown;
    }
    if (*requestsMade > IGMarketRequests::kMaxRequestsPerMinute) {
        return GateRefusal::RateLimited;
    }
    return std::nullopt;
}

namespace {

std::string_view refusalReason(const GateRefusal refusal) {
    switch (refusal) {
    case GateRefusal::DuplicateDeal:    return "duplicateDeal";
    case GateRefusal::DuplicateUnknown: return "duplicateUnknown";
    case GateRefusal::BudgetUnknown:    return "budgetUnknown";
    case GateRefusal::RateLimited:      return "rateLimit";
    }
    return "unknown";
}

}  // namespace

RequestOutcome IGMarketRequests::request(const std::string& path,
                                         const std::string& method,
                                         const std::string& jsonBody,
                                         const ig_rest::Headers& extraHeaders,
                                         const RequestOptions& options) {
    using backtest_log::logLine;

    // Gate reads first, then the pure policy. An empty dealKey has nothing
    // to deduplicate (definite false, C# behaviour) — only a real key pays
    // the Redis read.
    const std::optional<bool> duplicate =
        options.dealKey.empty() ? std::optional<bool>{false}
                                : gate_.isDuplicateDeal(options.dealKey);
    const int minute = currentMinuteOfHour();
    const std::optional<int> made = gate_.requestsMade(minute);

    if (const std::optional<GateRefusal> refusal =
            gatePolicy(duplicate, made, options.riskReducing)) {
        const std::string reason{refusalReason(*refusal)};
        logLine("IGMarketRequests: {} — refusing {} {} (dealKey={})",
                reason, method, path, options.dealKey);
        if (live_trace::enabled()) {
            if (*refusal == GateRefusal::RateLimited) {
                live_trace::emit("igRefused", {},
                                 {{"reason", reason},
                                  {"method", method},
                                  {"path", path},
                                  {"dealKey", options.dealKey},
                                  {"requestsThisMinute",
                                   static_cast<std::int64_t>(*made)}});
            } else {
                live_trace::emit("igRefused", {},
                                 {{"reason", reason},
                                  {"method", method},
                                  {"path", path},
                                  {"dealKey", options.dealKey}});
            }
        }
        return RequestOutcome{.fate = RequestFate::Refused,
                              .detail = reason};
    }

    // Record before sending, like the C# order: the budget must count an
    // attempt even when the exchange itself then fails. Best-effort — a
    // risk-reducing request proceeding through a Redis outage records
    // nothing, which the policy above already priced in.
    gate_.recordRequest(minute);
    gate_.recordDealRequest(options.dealKey);

    const std::optional<ig_rest::Auth> auth = auth_();
    if (!auth) {
        logLine("IGMarketRequests: no IG session credentials — refusing {} {}"
                " (is the login service writing Auth#<env> to DynamoDB, and "
                "are AWS credentials in the environment?)",
                method, path);
        if (live_trace::enabled()) {
            live_trace::emit("igRefused", {},
                             {{"reason", "noAuthSession"},
                              {"method", method},
                              {"path", path},
                              {"dealKey", options.dealKey}});
        }
        return RequestOutcome{.fate = RequestFate::Refused,
                              .detail = "noAuthSession"};
    }

    std::optional<ig_rest::HttpResponse> response =
        ig_rest::execute(*auth, path, method, jsonBody, extraHeaders,
                         options.transportRetries);
    if (!response) {
        return RequestOutcome{.fate = RequestFate::TransportFailed,
                              .detail = "transport failed"};
    }
    return RequestOutcome{.fate = RequestFate::Responded,
                          .response = std::move(response)};
}

PlaceOrder IGMarketCalls::makeOpen(RequestFn request,
                                   const int confirmAttempts,
                                   const std::chrono::milliseconds confirmDelay) {
    return [request = std::move(request), confirmAttempts, confirmDelay](
               const TradeOpenObj& order,
               const OrderContext& context) -> OpenResult {
        // transportRetries = 0: the open POST is NOT idempotent, so a lost
        // ACK is never resolved by re-sending — ambiguity goes through the
        // confirms poll below, keyed on the dealReference WE minted (it is
        // in the POST body, so IG can name the deal whether or not the
        // response reached us).
        const auto outcome = request(
            "/positions/otc", "POST", encodeTradeOpen(order), {},
            RequestOptions{.dealKey = context.strategyUuid
                                      + context.openDirection,
                           .riskReducing = false,
                           .transportRetries = 0});
        if (outcome.fate == RequestFate::Refused) {
            return OpenResult{.status = OpenStatus::Failed,
                              .reason = "refused: " + outcome.detail};
        }

        // Transport failure, 5xx and 408 all leave the same ambiguity: IG
        // may or may not hold the order. Ask /confirms instead of guessing.
        const bool ambiguous =
            outcome.fate == RequestFate::TransportFailed
            || outcome.response->status >= 500
            || outcome.response->status == 408;
        if (ambiguous) {
            const std::string how =
                outcome.fate == RequestFate::TransportFailed
                    ? outcome.detail
                    : "HTTP " + std::to_string(outcome.response->status);
            if (order.dealReference.empty()) {
                // Nothing to poll under — refuse to guess.
                return OpenResult{.status = OpenStatus::Failed,
                                  .reason = "ambiguous open (" + how
                                            + ") with no dealReference to "
                                              "confirm under — not re-sent"};
            }
            backtest_log::logLine(
                "IGMarketCalls: open POST ambiguous ({}) — resolving via "
                "confirms for {}",
                how, order.dealReference);
            if (const std::optional<OpenResult> resolved = pollConfirm(
                    request, order.dealReference, order.dealReference,
                    confirmAttempts, confirmDelay)) {
                return *resolved;
            }
            // No confirm after an ambiguous POST: treat as not placed and do
            // NOT re-send. The order-channel failure TTL brakes re-entry,
            // and if the order DID land the producer's book sync seeds it
            // within a refresh.
            return OpenResult{.status = OpenStatus::Failed,
                              .reason = "unconfirmed after ambiguous open ("
                                        + how + ") — not re-sent"};
        }

        const ig_rest::HttpResponse& response = *outcome.response;
        if (response.status != 200) {
            return OpenResult{.status = OpenStatus::Failed,
                              .reason = "HTTP "
                                        + std::to_string(response.status)
                                        + ": " + response.body};
        }
        const auto parsed = parsePositionResponse(response.body);
        if (!parsed) {
            return OpenResult{.status = OpenStatus::Failed,
                              .reason = "failed to parse response: "
                                        + response.body};
        }
        if (parsed->errorCode && !parsed->errorCode->empty()) {
            return OpenResult{.status = OpenStatus::Failed,
                              .reason = "errorCode: " + *parsed->errorCode};
        }

        // Confirm poll. IG's echoed reference names the deal; fall back to
        // the reference WE sent (same fallback the channel books under).
        // The Accepted result keeps reporting the raw echo, empty or not —
        // the channel compensates (existing behaviour, unchanged here).
        const std::string reference = parsed->dealReference.empty()
                                          ? order.dealReference
                                          : parsed->dealReference;
        if (const std::optional<OpenResult> resolved =
                pollConfirm(request, reference, parsed->dealReference,
                            confirmAttempts, confirmDelay)) {
            return *resolved;
        }
        backtest_log::logLine(
            "IGMarketCalls: confirm for {} never resolved after {} attempts "
            "— booking with an empty dealId (strategy closes blocked until "
            "an id appears)",
            reference, confirmAttempts);
        return OpenResult{.status = OpenStatus::Accepted,
                          .dealReference = parsed->dealReference};
    };
}

PlaceClose IGMarketCalls::makeClose(RequestFn request) {
    // The context is deliberately unused: the close is keyed on the dealId,
    // never the open's uuid+direction — sharing that key let a fresh open's
    // 30s marker refuse the close that followed it.
    return [request = std::move(request)](
               const TradeCloseObj& close,
               const OrderContext&) -> CloseResult {
        // Risk-reducing: gate uncertainty (Redis down) and the soft budget
        // never refuse a close; only its own close#<dealId> marker paces
        // repeats. Transport retries stay on — re-sending a close of the
        // same dealId is safe (IG rejects the second), unlike the open.
        const auto outcome = request(
            "/positions/otc", "POST", encodeTradeClose(close),
            {{"_method", "DELETE"}},
            RequestOptions{.dealKey = "close#" + close.dealId,
                           .riskReducing = true});
        if (outcome.fate == RequestFate::Refused) {
            // Never sent — the position is exactly where it was. Failed
            // keeps the book entry so the sync loop retries the close;
            // the old mapping (Gone) deleted the very Redis entry that
            // retry depends on.
            return CloseResult{.status = CloseStatus::Failed,
                               .reason = "refused: " + outcome.detail};
        }
        if (outcome.fate == RequestFate::TransportFailed) {
            // The C# null-response branch, now reserved for a REAL lost
            // exchange: as far as anyone can tell the position is missing
            // from IG — the caller deletes the book entry so a phantom
            // cannot haunt the strategy logic; the producer restores it
            // from the broker book if it does still exist.
            return CloseResult{.status = CloseStatus::Gone,
                               .reason = "no response after send ("
                                         + outcome.detail + ")"};
        }
        const ig_rest::HttpResponse& response = *outcome.response;
        if (response.status != 200) {
            return CloseResult{.status = CloseStatus::Failed,
                               .reason = "HTTP "
                                         + std::to_string(response.status)
                                         + ": " + response.body};
        }
        if (!parsePositionResponse(response.body)) {
            return CloseResult{.status = CloseStatus::Failed,
                               .reason = "failed to parse response: "
                                         + response.body};
        }
        return CloseResult{.status = CloseStatus::Ok};
    };
}

PlaceOrder IGMarketCalls::makeLiveOpen(std::string redisHost,
                                       const int redisPort,
                                       AuthProvider auth) {
    PlaceOrder core = makeOpen(threadLocalRequestFn(std::move(redisHost),
                                                    redisPort,
                                                    std::move(auth)));
    return [core = std::move(core)](const TradeOpenObj& order,
                                    const OrderContext& context) {
        const OpenResult result = core(order, context);
        if (result.status == OpenStatus::Accepted) {
            auditTrade("Open Trade", context, result.dealReference,
                       result.dealId);
        }
        return result;
    };
}

PlaceClose IGMarketCalls::makeLiveClose(std::string redisHost,
                                        const int redisPort,
                                        AuthProvider auth) {
    PlaceClose core = makeClose(threadLocalRequestFn(std::move(redisHost),
                                                     redisPort,
                                                     std::move(auth)));
    return [core = std::move(core)](const TradeCloseObj& close,
                                    const OrderContext& context) {
        const CloseResult result = core(close, context);
        if (result.status == CloseStatus::Ok) {
            auditTrade("Close Trade", context, close.dealId);
        }
        return result;
    };
}

}  // namespace ig
