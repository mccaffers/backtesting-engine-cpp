// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// ingestCommand — the `ingest` subcommand. Receives tick datagrams over UDP,
// decodes them (tickPacket) and persists each into the local QuestDB via ILP
// over HTTP (questdbIngestClient), so the backtester's `run` path can read them
// back through the unchanged DatabaseConnection.
//
// The GMF only #includes Asio-/curl-free headers (the receiver and writer hide
// those behind pimpls), so it is safe to `import std` here.

module;

#include "ingest/questdbIngestClient.hpp"
#include "ingest/udpPorts.hpp"
#include "ingest/udpReceiver.hpp"
#include "shared/utilities/env.hpp"

export module ingestCommand;

import std;
import priceData;   // PriceData
import tickPacket;  // ingest::decodeTick

export class IngestCommand {
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

}  // namespace

int IngestCommand::run(const int argc, const char* argv[]) {
    const std::string questHost = env::getOr("QUESTDB_HOST", "127.0.0.1");
    // 9001 is the converted (INT32) QuestDB's HTTP/ILP port — the instance the
    // backtester reads and that convert.py writes to. (9000 is the legacy DOUBLE
    // source DB.) Override with $QUESTDB_ILP_PORT.
    const std::uint16_t ilpPort = parsePort(env::getOr("QUESTDB_ILP_PORT", ""), 9001);
    const std::string bindAddr = env::getOr("INGEST_BIND_ADDR", "127.0.0.1");

    // Bind port: CLI arg (argv[2]) overrides $INGEST_UDP_PORT overrides kSave.
    std::uint16_t bindPort = parsePort(env::getOr("INGEST_UDP_PORT", ""), udp_ports::kSave);
    if (argc >= 3) {
        bindPort = parsePort(argv[2], bindPort);
    }

    ingest::QuestdbIngestClient writer(questHost, ilpPort);

    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> dropped{0};

    ingest::UdpReceiver receiver(
        bindAddr, bindPort, [&](std::span<const std::byte> bytes) {
            const auto tick = ingest::decodeTick(bytes);
            if (!tick) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // ILP line: table = symbol, integer fields ask/bid, designated
            // timestamp in nanoseconds (QuestDB HTTP default precision). This
            // yields exactly the ask/bid/timestamp columns the read path expects.
            const auto tsNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     tick->timestamp.time_since_epoch())
                                     .count();
            writer.enqueueLine(std::format("{} ask={}i,bid={}i {}\n",
                                           tick->symbol, tick->ask, tick->bid, tsNanos));
            received.fetch_add(1, std::memory_order_relaxed);
        });

    std::println("IngestCommand: starting; binding udp://{}:{} -> QuestDB ILP http://{}:{}/write",
                 bindAddr, bindPort, questHost, ilpPort);

    if (!receiver.run()) {  // binds the socket, then blocks until SIGINT/SIGTERM
        return 1;           // bind failed (bad address / port in use) — logged
    }

    // decode-dropped: bad size / unknown symbol. queue-dropped: shed by the
    // writer when QuestDB couldn't keep up and the buffer hit its cap.
    std::println("IngestCommand: shutting down (received={}, decode-dropped={}, queue-dropped={})",
                 received.load(), dropped.load(), writer.droppedLines());
    return 0;
}
