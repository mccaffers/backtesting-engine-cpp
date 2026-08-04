// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// trackingCommand — the `tracking` subcommand. Receives the IG markets
// producer's trade-positioning updates (256-byte deal datagrams, see
// dealPacket) over UDP and, per deal, mirrors the C# trade-tracking service
// (vortex/trade_tracking/Program.cs): look up the originating position in
// Redis (PH# history first, PO# live fallback), compute the close pips for a
// DELETED deal, archive its book entry (PO# -> PH#, pruning the PL# list —
// the one step the C# left to expiry, which lost broker-stop closes), and
// queue the ElasticTradeLogs document to "live-trades". The pure pieces
// (lookup order, pip math, doc shape) live in trackingReport; this file is
// the wiring, structured like ingestCommand around the shared
// net::UdpReceiver.
//
// The GMF only #includes Asio-free headers (the receiver, Redis and Elastic
// machinery all hide their transports behind .cpp files), so it is safe to
// `import std` here.

module;

#include "run/reporting/elasticPublisher.hpp"
#include "shared/net/udpPorts.hpp"
#include "shared/net/udpReceiver.hpp"
#include "shared/redis/positionManager.hpp"
#include "shared/utilities/backtestLog.hpp"  // backtest_log::error
#include "shared/utilities/env.hpp"

export module trackingCommand;

import std;
import backtestLog;        // backtest_log::logLine — timestamped, flushed stdout
import dealPacket;         // deal_packet::decodeDeal
import marketDefinitions;  // live::findMarketByEpicMini — epic -> symbol fallback
import symbolScale;        // symbol_scale::get / getPriceScale
import trackingReport;     // the pure lookup/pip/document helpers

export class TrackingCommand {
public:
    static int run(int argc, const char* argv[]);
};

namespace {

// Parse a port from a string, returning `fallback` on empty/garbage input so a
// stray env var can't crash startup.
std::uint16_t parsePort(std::string_view text, std::uint16_t fallback) {
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value == 0 || value > 65535) {
        return fallback;
    }
    return static_cast<std::uint16_t>(value);
}

// The wire marks an absent double as NaN (surfaced here as nullopt); log it as
// "-" so an unset stop/limit reads as deliberately absent, not zero.
std::string fmtOptional(const std::optional<double>& value) {
    return value ? std::format("{}", *value) : std::string("-");
}

}  // namespace

int TrackingCommand::run(const int argc, const char* argv[]) {
    using backtest_log::logLine;

    const std::string bindAddr = env::getOr("TRACKING_BIND_ADDR", "127.0.0.1");

    // Bind port: CLI arg (argv[2]) overrides $TRACKING_UDP_PORT overrides kTrade.
    std::uint16_t bindPort = parsePort(env::getOr("TRACKING_UDP_PORT", ""), udp_ports::kTrade);
    if (argc >= 3) {
        bindPort = parsePort(argv[2], bindPort);
    }

    const std::string redisHost = env::getOr("REDIS_HOST", "127.0.0.1");
    constexpr int redisPort = 6379;  // by convention, as positionsCommand
    std::string tradingEnv = env::getOr("TRADING_ENVIRONMENT", "demo");
    std::ranges::transform(tradingEnv, tradingEnv.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // Not thread-safe, but the receiver invokes the handler on its run()
    // thread only, so one instance in this scope is the whole story. Redis
    // being unreachable degrades to fallback documents, never a crash: the
    // getters return outer-nullopt (logged) and lookupPosition misses.
    redis_positions::PositionManager positions(redisHost, redisPort);

    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> dropped{0};

    // The enrichment pipeline for one decoded deal — the C# receive-loop
    // body. Split from the receiver lambda so the handler reads as
    // decode-log-report.
    const auto report = [&](const deal_packet::Deal& deal) {
        const auto record = tracking_report::lookupPosition(
            deal.dealReference,
            [&](const std::string& ref) {
                return positions.getHistoryPositionPayload(ref);
            },
            [&](const std::string& ref) {
                return positions.getPositionPayload(ref);
            });
        if (!record) {
            logLine("TrackingCommand: no PH#/PO# record for ref={} — writing "
                    "doc with fallbacks",
                    deal.dealReference);
        }

        // Last-resort symbol when the book misses: map the epic back through
        // marketDefinitions, else ship the raw epic (the C# used deal.Epic).
        std::string fallbackSymbol = deal.epic;
        if (const auto* market = live::findMarketByEpicMini(deal.epic)) {
            fallbackSymbol = std::string(market->symbol);
        }

        // The C# pip calculator: only a DELETED (closed) deal with a known
        // position and a real close level. Sign and size come from the
        // position — the deal's own direction is the closing side.
        std::optional<double> pips;
        if (record && deal.level && *deal.level != 0.0
            && deal.status == "DELETED") {
            const std::string& symbol =
                !record->symbol.empty() ? record->symbol : fallbackSymbol;
            pips = tracking_report::computeClosePips(
                *deal.level, record->level, record->direction, record->size,
                symbol_scale::getPriceScale(symbol), symbol_scale::get(symbol));
            if (pips) {
                logLine("TrackingCommand: {} TRADE UPDATE — close pips: "
                        "({} - {} points) for {} size={} => {} pips",
                        symbol, *deal.level, record->level, record->direction,
                        record->size, *pips);
            } else {
                logLine("TrackingCommand: unknown scale for symbol {} — "
                        "skipping pip calc",
                        symbol);
            }
        }

        // Archive the book entry the moment the broker says DELETED: PO# ->
        // PH# (60 days) and out of the PL# list. Idempotent when the
        // strategy-close path already moved it (a missing PO# skips the PH#
        // write). An empty strategyId would address a malformed PL# key.
        if (record && deal.status == "DELETED" && !record->strategyId.empty()) {
            const bool archived =
                positions.removePosition(record->strategyId, deal.dealReference);
            logLine("TrackingCommand: archived ref={} PO#->PH# for strategy "
                    "{} (ok={})",
                    deal.dealReference, record->strategyId, archived);
        }

        elastic::enqueueDocument(
            "live-trades",
            tracking_report::buildLiveTradeDocument(deal, record, fallbackSymbol,
                                                    tradingEnv,
                                                    elastic::nowIsoUtc(), pips));
    };

    net::UdpReceiver receiver(
        bindAddr, bindPort, [&](std::span<const std::byte> bytes) {
            const auto deal = deal_packet::decodeDeal(bytes);
            if (!deal) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            received.fetch_add(1, std::memory_order_relaxed);
            // Deals are account-level events (a handful per day, not a tick
            // stream), so unlike ingest each one is logged in full — no
            // throughput reporter needed.
            logLine("TrackingCommand: {} {} {} {} size={} level={} stop={} "
                    "limit={} dealId={} ref={} origin={} channel={} expiry={} "
                    "currency={} guaranteedStop={} ts={}",
                    deal->status, deal->dealStatus, deal->epic, deal->direction,
                    fmtOptional(deal->size), fmtOptional(deal->level),
                    fmtOptional(deal->stopLevel), fmtOptional(deal->limitLevel),
                    deal->dealId, deal->dealReference, deal->dealIdOrigin,
                    deal->channel, deal->expiry, deal->currency,
                    deal->guaranteedStop, deal->timestamp);
            // A surprise from the report path (Redis payload, serialization)
            // must not take the receive loop down — log and move on, like the
            // C# swallow-and-log.
            try {
                report(*deal);
            } catch (const std::exception& e) {
                backtest_log::error(std::format(
                    "TrackingCommand: reporting ref={} failed ({})",
                    deal->dealReference, e.what()));
            } catch (...) {
                backtest_log::error(std::format(
                    "TrackingCommand: reporting ref={} failed (non-std "
                    "exception)",
                    deal->dealReference));
            }
        });

    logLine("TrackingCommand: starting; binding udp://{}:{} for {}-byte deal "
            "packets ([{}], redis {}:{})",
            bindAddr, bindPort, deal_packet::kPacketSize, tradingEnv, redisHost,
            redisPort);

    const bool ok = receiver.run();  // binds the socket, then blocks until SIGINT/SIGTERM
    if (!ok) {
        return 1;  // bind failed (bad address / port in use) — logged
    }

    // dropped: datagrams that weren't exactly 256 bytes (stray/garbled traffic).
    logLine("TrackingCommand: shutting down (received={}, dropped={})",
            received.load(), dropped.load());
    // Deliver the reporting tail now, deterministically, rather than leaving
    // it to the publisher's exit-time flush.
    elastic::flushQueuedDocuments();
    return 0;
}
