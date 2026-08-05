// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// positionsCommand — the `positions` subcommand: the IG position producer,
// replacing the external C# igmarkets_positions cron. Where the cron ran
// once a minute under an external scheduler, this process loops forever and
// paces itself: the first sync fires immediately (cron parity), then one
// cycle per minute, each cycle mirroring the broker's /positions book into
// Redis (PO#/PL#/CG#) via positionSync.
//
// Loop shape: the deadline is re-armed AFTER each cycle (next = now + 1min,
// cron semantics) so a slow IG exchange — worst case ~90s with retries —
// never causes a catch-up burst; the wait sleeps in 1-second ticks so
// SIGINT/SIGTERM (a plain signal flag; no UDP receiver owns the signals
// here) is honoured within ~1s. Deliberately NOT std::jthread + stop_token:
// its condition_variable_any wait fails to link under `import std` (see the
// module-migration notes / liveReporter).
//
// `positions --once` runs a single cycle and exits — the literal C# cron
// behaviour, and the smoke-test mode.

module;

#include <csignal>  // SIGINT/SIGTERM are macros — `import std` can't supply them

#include "run/reporting/elasticPublisher.hpp"
#include "shared/utilities/env.hpp"

export module positionsCommand;

import std;
import backtestLog;   // backtest_log::logLine
import igRequests;    // ig::dynamoAuthProvider — Auth#<env> from DynamoDB
import positionSync;  // positions::Sync — one producer cycle

export class PositionsCommand {
public:
    static int run(int argc, const char* argv[]);
};

namespace {

constexpr std::chrono::minutes kInterval{1};

// Written from the signal handler: atomic<bool> is lock-free on the deploy
// targets, which makes the relaxed store async-signal-safe.
std::atomic<bool> stopRequested{false};

void onSignal(int) { stopRequested.store(true, std::memory_order_relaxed); }

}  // namespace

int PositionsCommand::run(const int argc, const char* argv[]) {
    using backtest_log::logLine;

    const std::string redisHost = env::getOr("REDIS_HOST", "127.0.0.1");
    constexpr int redisPort = 6379;  // by convention, as liveSettings
    std::string tradingEnv = env::getOr("TRADING_ENVIRONMENT", "demo");
    std::ranges::transform(tradingEnv, tradingEnv.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const bool runOnce =
        argc >= 3 && std::string_view{argv[2]} == "--once";

    // One probe pull at startup, loud like liveCommand: without a session
    // every cycle files a FAILED-REQUEST report and touches nothing — safe,
    // but the operator should hear it before the first minute, not from
    // Kibana. Auth is re-pulled per cycle, so a later login-service refresh
    // is picked up without a restart.
    const ig::AuthProvider auth = ig::dynamoAuthProvider(tradingEnv);
    const bool haveAuth = auth().has_value();
    if (!haveAuth) {
        logLine("PositionsCommand: WARNING — could not pull IG session "
                "Auth#{} from DynamoDB ({} table); every cycle will skip "
                "until a session appears (check AWS credentials in the "
                "environment and the login service)",
                tradingEnv, "MarketDataLive");
    }

    positions::Sync sync(
        positions::Sync::Config{.redisHost = redisHost,
                                .redisPort = redisPort,
                                .tradingEnv = tradingEnv},
        auth);

    logLine("PositionsCommand: starting; IG /positions -> Redis PO#/PL#/CG# "
            "{} ([{}]{}, redis {}:{})",
            runOnce ? "once (--once)"
                    : std::format("every {}s", std::chrono::seconds{kInterval}
                                                   .count()),
            tradingEnv, haveAuth ? "" : " WITHOUT a session", redisHost,
            redisPort);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::uint64_t cycles = 0;
    for (;;) {
        sync.syncOnce();  // never throws; a failed cycle waits for the next
        ++cycles;
        if (runOnce) {
            break;
        }
        // A signal during a cycle (the IG exchange can run tens of seconds)
        // is honoured here, before any wait.
        const auto next = std::chrono::steady_clock::now() + kInterval;
        while (!stopRequested.load(std::memory_order_relaxed)
               && std::chrono::steady_clock::now() < next) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
        if (stopRequested.load(std::memory_order_relaxed)) {
            break;
        }
    }

    logLine("PositionsCommand: shutting down (cycles={})", cycles);
    // Deliver the reporting tail now, deterministically, rather than leaving
    // it to the publisher's exit-time flush.
    elastic::flushQueuedDocuments();
    return 0;
}
