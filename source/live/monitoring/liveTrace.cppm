// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveTrace — structured trace documents for live mode's important junctions
// (order/close lifecycle, gate blocks, book sync, IG guard refusals,
// startup/shutdown, minutely stats), delivered to the Elasticsearch index
// "live-traces" through the shared async batch publisher (elasticPublisher:
// background flusher, retry + dead-letter, never blocks the caller).
//
// Tracing is ARMED, not merely env-gated: every emit is a no-op until
// init() runs, and only LiveCommand::run calls init(). Unit tests construct
// OrderChannel / StrategyRunner / the sinks directly and never init, so
// their trace calls cost one relaxed atomic load — no batcher thread, no
// delivery attempt, no dead-letter file in the test dir. init() arms unless
// $LIVE_TRACE_ENABLED == "0" (default on), and records TRADING_ENVIRONMENT
// and the hostname regardless, so igRequests' auditTrade can stamp `env`
// on live-trades documents even with tracing itself switched off.
//
// Call sites pass typed Fields rather than JSON, so no caller needs
// <nlohmann/json.hpp> in its GMF and nothing is serialised while disarmed.
// Convention: `reason` values are static keywords (Kibana-aggregatable);
// dynamic text (broker error bodies, exception messages) goes in `detail`.
//
// init() also registers the backtest_log sink (unless $LIVE_LOG_SHIP_ENABLED
// == "0"), shipping every error()/logLine() line as a "live-logs" document
// through the same batcher — so the human-readable narrative survives off
// the box alongside the structured traces. The publisher suppresses its own
// lines from the sink (backtestLog.hpp) so a delivery outage cannot feed
// itself.
//
// The GMF includes are all Asio-free headers, so `import std` is safe here.

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"  // elastic::enqueueDocument
#include "run/reporting/tradeDocument.hpp"     // TradeDocument::isoUtcMillis
#include "run/reporting/tradingResults.hpp"    // TradeFinal::localHostname
#include "shared/utilities/backtestLog.hpp"    // backtest_log::error
#include "shared/utilities/env.hpp"            // env::getOr

export module liveTrace;

import std;

export namespace live_trace {

// Correlation ids common to most events; empty ones are omitted from the
// document. The views are copied during emit, so they only need to outlive
// the call.
struct Common {
    std::string_view strategyUuid{};
    std::string_view strategyName{};
    std::string_view symbol{};
    std::string_view dealReference{};
    std::string_view dealId{};
};

// One event-specific field. The variant covers every scalar the event
// taxonomy needs; string values land as JSON strings.
struct Field {
    std::string_view key;
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string_view>
        value;
};

// Called once from LiveCommand::run, on the main thread, before any worker/
// receiver/reporter thread exists (the env/hostname strings are written here
// and read-only afterwards — thread creation provides the happens-before).
// `tradingEnv` is Settings::fromEnv's already-lowercased TRADING_ENVIRONMENT,
// passed in rather than re-read so the doc field can never disagree with the
// Auth#<env> credentials actually in use.
void init(std::string tradingEnv);

// Switch tracing AND log shipping back off — the full reset for tests that
// exercise init(); production never disarms.
void disarm();

// One relaxed atomic load. Call sites wrap emits in `if (enabled())` so a
// disarmed trace also skips evaluating its field expressions.
[[nodiscard]] bool enabled();

// The cached environment name ("demo"/"live"); "unknown" before init().
[[nodiscard]] std::string_view tradingEnv();

// Build the document body (exported for unit tests; deterministic apart from
// @timestamp and hostname). Envelope: @timestamp (UTC, milliseconds — the
// seconds-only stamps used elsewhere would collapse a placement and its
// confirm onto one instant), env, hostname, event, the non-empty Common ids,
// then the fields, all at top level.
[[nodiscard]] std::string buildDocumentJson(std::string_view event,
                                            const Common& common,
                                            std::initializer_list<Field> fields);

// One live-logs document (exported for unit tests): @timestamp/env/hostname
// like the traces, level "error"|"info" (stderr vs stdout origin), and the
// line itself under `message`, capped so a rogue dump cannot bloat the
// index (the cap is generous — normal lines are a few hundred bytes).
[[nodiscard]] std::string buildLogDocumentJson(bool isError,
                                               std::string_view message);

// No-op unless armed; otherwise queue the document for "live-traces". Never
// blocks on the network and never throws (same doctrine as
// elastic::putEngineException): serialisation failures are logged to stderr
// and swallowed — a trace must never take down an order path.
void emit(std::string_view event, const Common& common,
          std::initializer_list<Field> fields = {}) noexcept;

}  // namespace live_trace

namespace live_trace {

namespace {

std::atomic<bool>& armedFlag() {
    static std::atomic<bool> armed{false};
    return armed;
}

std::string& envName() {
    static std::string name = "unknown";
    return name;
}

std::string& hostName() {
    static std::string host = "unknown";
    return host;
}

}  // namespace

namespace {

// The backtest_log sink (capture-free — the atomic slot holds a plain
// function pointer). Same never-throw doctrine as emit(): a log line must
// never take down the code that logged it.
void shipLogLine(const bool isError, const std::string_view message) noexcept {
    try {
        elastic::enqueueDocument("live-logs",
                                 buildLogDocumentJson(isError, message));
    } catch (...) {
        // Deliberately NOT backtest_log::error — that is the very function
        // whose sink just failed; stderr via the mutex-free path would still
        // recurse through shipToSink. Swallow: the line already reached
        // stdout/stderr.
    }
}

}  // namespace

void init(std::string tradingEnv) {
    envName() = std::move(tradingEnv);
    hostName() = TradeFinal::localHostname();
    armedFlag().store(env::getOr("LIVE_TRACE_ENABLED", "1") != "0",
                      std::memory_order_release);
    // Independent of the trace flag: the narrative and the structured
    // events are separate feeds with separate kill switches. The gate is
    // authoritative in both directions — "off" also clears any sink a
    // previous init registered, so a re-init cannot leave a stale sink
    // shipping.
    if (env::getOr("LIVE_LOG_SHIP_ENABLED", "1") != "0") {
        backtest_log::setSink(&shipLogLine);
    } else {
        backtest_log::setSink(nullptr);
    }
}

void disarm() {
    armedFlag().store(false, std::memory_order_release);
    backtest_log::setSink(nullptr);
}

bool enabled() { return armedFlag().load(std::memory_order_relaxed); }

std::string_view tradingEnv() { return envName(); }

std::string buildDocumentJson(const std::string_view event,
                              const Common& common,
                              const std::initializer_list<Field> fields) {
    nlohmann::json doc{
        {"@timestamp",
         TradeDocument::isoUtcMillis(std::chrono::system_clock::now())},
        {"env", envName()},
        {"hostname", hostName()},
        {"event", std::string(event)},
    };
    const auto putId = [&doc](const char* key, const std::string_view value) {
        if (!value.empty()) {
            doc[key] = std::string(value);
        }
    };
    putId("strategyUuid", common.strategyUuid);
    putId("strategyName", common.strategyName);
    putId("symbol", common.symbol);
    putId("dealReference", common.dealReference);
    putId("dealId", common.dealId);
    for (const Field& field : fields) {
        std::visit(
            [&doc, &field](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string_view>) {
                    // Same convention as the Common ids: an empty string
                    // (blank dealKey, empty broker reason) is omitted, not
                    // indexed as "".
                    if (!value.empty()) {
                        doc[std::string(field.key)] = std::string(value);
                    }
                } else {
                    doc[std::string(field.key)] = value;
                }
            },
            field.value);
    }
    return doc.dump();
}

std::string buildLogDocumentJson(const bool isError,
                                 const std::string_view message) {
    // Generous cap: protects the index from a pathological line (a dumped
    // document, a runaway body) without touching normal traffic.
    constexpr std::size_t kMaxMessageBytes = 4096;
    const nlohmann::json doc{
        {"@timestamp",
         TradeDocument::isoUtcMillis(std::chrono::system_clock::now())},
        {"env", envName()},
        {"hostname", hostName()},
        {"level", isError ? "error" : "info"},
        {"message", std::string(message.substr(0, kMaxMessageBytes))},
    };
    // error_handler_t::replace: a log line can carry arbitrary bytes (broker
    // response bodies, exception text); U+FFFD beats throwing the doc away.
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void emit(const std::string_view event, const Common& common,
          const std::initializer_list<Field> fields) noexcept {
    if (!enabled()) {
        return;
    }
    try {
        elastic::enqueueDocument("live-traces",
                                 buildDocumentJson(event, common, fields));
    } catch (...) {
        backtest_log::error("liveTrace: failed to build/queue a trace "
                            "document — dropping it");
    }
}

}  // namespace live_trace
