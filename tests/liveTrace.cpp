// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveTrace: the armed gate (inert until init(), LIVE_TRACE_ENABLED=0 keeps
// it disarmed, the environment is recorded either way) and the document
// envelope (@timestamp to the millisecond, env/hostname/event, empty
// correlation ids and empty string fields omitted, each Field alternative
// serialising as its JSON type). The machinery only — no pinned config
// values, and nothing here arms tracing without disarming again (an armed
// gate in another TU's tests would try to deliver documents at process
// exit).
//
// ELASTIC_ENABLED=0 is set before every init() as a second fence: even if an
// emit slipped through while armed, the publisher would drop it instead of
// spinning up its flusher against a dead localhost.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "shared/utilities/backtestLog.hpp"

import backtestLog;  // backtest_log::logLine — the stdout half of the sink
import liveTrace;

namespace {

// "YYYY-MM-DDTHH:MM:SS.mmmZ" — 24 chars, digits everywhere but the fixed
// separators.
bool isIsoUtcMillis(const std::string& ts) {
    if (ts.size() != 24) {
        return false;
    }
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const char c = ts[i];
        switch (i) {
        case 4:
        case 7:
            if (c != '-') return false;
            break;
        case 10:
            if (c != 'T') return false;
            break;
        case 13:
        case 16:
            if (c != ':') return false;
            break;
        case 19:
            if (c != '.') return false;
            break;
        case 23:
            if (c != 'Z') return false;
            break;
        default:
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
            break;
        }
    }
    return true;
}

}  // namespace

// Declared first in this TU on purpose: in a single-process run it must see
// the gate before any test below arms it (ctest runs each case in its own
// process anyway).
TEST_CASE("tracing is inert before init", "[liveTrace]") {
    CHECK_FALSE(live_trace::enabled());
    CHECK(live_trace::tradingEnv() == "unknown");
    // emit while disarmed is a no-op — must not throw or touch the publisher.
    live_trace::emit("orderIntent", {}, {{"size", std::int64_t{3}}});
    CHECK_FALSE(live_trace::enabled());
}

TEST_CASE("init arms tracing and records the environment", "[liveTrace]") {
    ::setenv("ELASTIC_ENABLED", "0", 1);
    ::unsetenv("LIVE_TRACE_ENABLED");  // default: on

    live_trace::init("demo");
    CHECK(live_trace::enabled());
    CHECK(live_trace::tradingEnv() == "demo");
    // The default-on ship gate registers the live-logs sink too.
    CHECK(backtest_log::sinkArmed());
    // An armed emit routes to the (disabled) publisher without throwing.
    live_trace::emit("startup", {}, {{"workers", std::uint64_t{2}}});

    live_trace::disarm();
    CHECK_FALSE(live_trace::enabled());
    CHECK_FALSE(backtest_log::sinkArmed());
}

TEST_CASE("LIVE_TRACE_ENABLED=0 keeps tracing disarmed but records the env",
          "[liveTrace]") {
    ::setenv("ELASTIC_ENABLED", "0", 1);
    ::setenv("LIVE_TRACE_ENABLED", "0", 1);

    live_trace::init("live");
    CHECK_FALSE(live_trace::enabled());
    // The env is stored regardless, for the live-trades audit stamp.
    CHECK(live_trace::tradingEnv() == "live");

    ::unsetenv("LIVE_TRACE_ENABLED");
    live_trace::disarm();
}

TEST_CASE("document envelope carries timestamp, env, hostname and event",
          "[liveTrace]") {
    ::setenv("ELASTIC_ENABLED", "0", 1);
    ::setenv("LIVE_TRACE_ENABLED", "0", 1);  // record env without arming
    live_trace::init("demo");
    ::unsetenv("LIVE_TRACE_ENABLED");

    const auto doc = nlohmann::json::parse(
        live_trace::buildDocumentJson("orderAccepted", {}, {}));

    CHECK(isIsoUtcMillis(doc.at("@timestamp").get<std::string>()));
    CHECK(doc.at("env").get<std::string>() == "demo");
    CHECK_FALSE(doc.at("hostname").get<std::string>().empty());
    CHECK(doc.at("event").get<std::string>() == "orderAccepted");

    live_trace::disarm();
}

TEST_CASE("non-empty correlation ids are indexed, empty ones omitted",
          "[liveTrace]") {
    const auto doc = nlohmann::json::parse(live_trace::buildDocumentJson(
        "closeOk",
        {.strategyUuid = "f47ac10b-58cc-4372-a567-0e02b2c3d479",
         .strategyName = "StubStrategy",
         .symbol = "EURUSD",
         .dealReference = "f47ac10b-L1719360000000"},
        {}));

    CHECK(doc.at("strategyUuid").get<std::string>()
          == "f47ac10b-58cc-4372-a567-0e02b2c3d479");
    CHECK(doc.at("strategyName").get<std::string>() == "StubStrategy");
    CHECK(doc.at("symbol").get<std::string>() == "EURUSD");
    CHECK(doc.at("dealReference").get<std::string>()
          == "f47ac10b-L1719360000000");
    CHECK_FALSE(doc.contains("dealId"));  // empty -> omitted
}

TEST_CASE("each field alternative serialises as its JSON type",
          "[liveTrace]") {
    const auto doc = nlohmann::json::parse(live_trace::buildDocumentJson(
        "stats", {},
        {{"flag", true},
         {"delta", std::int64_t{-7}},
         {"count", std::uint64_t{42}},
         {"score", 1.5},
         {"reason", "rateCap"},
         {"empty", std::string_view{}}}));

    CHECK(doc.at("flag").is_boolean());
    CHECK(doc.at("flag").get<bool>() == true);
    CHECK(doc.at("delta").is_number_integer());
    CHECK(doc.at("delta").get<std::int64_t>() == -7);
    CHECK(doc.at("count").is_number_unsigned());
    CHECK(doc.at("count").get<std::uint64_t>() == 42);
    CHECK(doc.at("score").is_number_float());
    CHECK(doc.at("score").get<double>() == 1.5);
    CHECK(doc.at("reason").is_string());
    CHECK(doc.at("reason").get<std::string>() == "rateCap");
    // Empty string fields follow the empty-id convention: omitted, not "".
    CHECK_FALSE(doc.contains("empty"));
}

TEST_CASE("log documents carry the shared envelope, a level and the message",
          "[liveTrace]") {
    ::setenv("ELASTIC_ENABLED", "0", 1);
    ::setenv("LIVE_TRACE_ENABLED", "0", 1);
    ::setenv("LIVE_LOG_SHIP_ENABLED", "0", 1);  // envelope only, no sink
    live_trace::init("demo");
    ::unsetenv("LIVE_TRACE_ENABLED");
    ::unsetenv("LIVE_LOG_SHIP_ENABLED");

    const auto errorDoc = nlohmann::json::parse(
        live_trace::buildLogDocumentJson(true, "OrderChannel: it broke"));
    CHECK(isIsoUtcMillis(errorDoc.at("@timestamp").get<std::string>()));
    CHECK(errorDoc.at("env").get<std::string>() == "demo");
    CHECK_FALSE(errorDoc.at("hostname").get<std::string>().empty());
    CHECK(errorDoc.at("level").get<std::string>() == "error");
    CHECK(errorDoc.at("message").get<std::string>()
          == "OrderChannel: it broke");

    const auto infoDoc =
        nlohmann::json::parse(live_trace::buildLogDocumentJson(false, "ok"));
    CHECK(infoDoc.at("level").get<std::string>() == "info");

    // The cap: a pathological line is truncated, not dropped or indexed raw.
    const std::string huge(10000, 'x');
    const auto cappedDoc =
        nlohmann::json::parse(live_trace::buildLogDocumentJson(false, huge));
    CHECK(cappedDoc.at("message").get<std::string>().size() == 4096);

    live_trace::disarm();
}

namespace {

// Collecting sink for the registration tests — capture-free, matching
// backtest_log::Sink; state lives in the accessor's static.
std::vector<std::pair<bool, std::string>>& collectedLines() {
    static std::vector<std::pair<bool, std::string>> lines;
    return lines;
}

void collectingSink(const bool isError, const std::string_view message) {
    collectedLines().emplace_back(isError, std::string{message});
}

}  // namespace

TEST_CASE("the backtest_log sink receives error and logLine text, honours "
          "suppression, and disarms",
          "[liveTrace]") {
    collectedLines().clear();
    backtest_log::setSink(&collectingSink);

    backtest_log::error("stderr line");
    backtest_log::logLine("stdout {} {}", "line", 7);
    REQUIRE(collectedLines().size() == 2);
    CHECK(collectedLines()[0] == std::pair{true, std::string{"stderr line"}});
    CHECK(collectedLines()[1] == std::pair{false, std::string{"stdout line 7"}});

    // The publisher's delivery paths log under this guard — nothing ships.
    {
        const backtest_log::SinkSuppression suppression;
        backtest_log::error("publisher-origin line");
    }
    CHECK(collectedLines().size() == 2);

    // Suppression is scoped: shipping resumes when the guard leaves.
    backtest_log::error("after the guard");
    CHECK(collectedLines().size() == 3);

    backtest_log::setSink(nullptr);
    backtest_log::error("after disarm");
    CHECK(collectedLines().size() == 3);
}

TEST_CASE("LIVE_LOG_SHIP_ENABLED=0 keeps the live-logs sink unregistered "
          "and clears a previously-armed one",
          "[liveTrace]") {
    ::setenv("ELASTIC_ENABLED", "0", 1);
    ::setenv("LIVE_TRACE_ENABLED", "0", 1);
    ::setenv("LIVE_LOG_SHIP_ENABLED", "0", 1);

    // Register BEFORE init: a gate-off init must clear the slot (the gate is
    // authoritative in both directions), and must not install shipLogLine.
    // Observing the slot directly (sinkArmed) catches the register-anyway
    // regression; the collecting sink catches the fails-to-clear one.
    collectedLines().clear();
    backtest_log::setSink(&collectingSink);
    live_trace::init("demo");
    CHECK_FALSE(backtest_log::sinkArmed());
    backtest_log::error("into the void");
    CHECK(collectedLines().empty());

    ::unsetenv("LIVE_TRACE_ENABLED");
    ::unsetenv("LIVE_LOG_SHIP_ENABLED");
    live_trace::disarm();
}
