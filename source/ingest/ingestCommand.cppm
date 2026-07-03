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

#include <ctime>  // POSIX gmtime_r (not exported by `import std`)

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

// UTC wall-clock prefix, e.g. "[2026-06-28 15:04:20.785]", to millisecond
// precision. gmtime_r is POSIX (hence the <ctime> include in the global module
// fragment) — the same approach tradeManager.cppm uses for UTC tick times.
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000;
    std::tm utc{};
    gmtime_r(&t, &utc);
    return std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}]",
                       utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
}

// Like std::println to stdout, but timestamped and flushed immediately. stdout
// is fully buffered when redirected (pipe/file/service log), so without the
// flush the once-a-minute reports below would sit in the buffer and only appear
// in a burst when the process exits.
template <class... Args>
void logLine(std::format_string<Args...> fmt, Args&&... args) {
    std::println("{} {}", timestamp(), std::format(fmt, std::forward<Args>(args)...));
    std::fflush(stdout);
}

}  // namespace

int IngestCommand::run(const int argc, const char* argv[]) {
    const std::string questHost = env::getOr("QUESTDB_HOST", "127.0.0.1");
    // QuestDB serves ILP-over-HTTP (POST /write) on its main HTTP port, 9000 by
    // default. Override with $QUESTDB_ILP_PORT.
    const std::uint16_t ilpPort = parsePort(env::getOr("QUESTDB_ILP_PORT", ""), 9000);
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

    logLine("IngestCommand: starting; binding udp://{}:{} -> QuestDB ILP http://{}:{}/write",
            bindAddr, bindPort, questHost, ilpPort);

    // receiver.run() blocks the main thread, so a background thread prints
    // throughput once a minute. It wakes every second to notice shutdown
    // promptly. Plain std::thread + atomic flag on purpose — std::jthread's
    // stop_token wait (condition_variable_any) doesn't link under `import std`
    // here (see module-migration notes).
    std::atomic<bool> reporting{true};
    std::thread reporter([&] {
        using clock = std::chrono::steady_clock;
        constexpr auto interval = std::chrono::minutes(1);
        std::uint64_t lastReceived = 0;
        auto nextReport = clock::now() + interval;
        while (reporting.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (clock::now() < nextReport) {
                continue;
            }
            nextReport += interval;
            const auto total = received.load(std::memory_order_relaxed);
            const auto delta = total - lastReceived;
            lastReceived = total;
            logLine("IngestCommand: received={} (+{}, ~{}/s); "
                    "decode-dropped={}, queue-dropped={}, post-failed={}",
                    total, delta, delta / 60,
                    dropped.load(std::memory_order_relaxed), writer.droppedLines(),
                    writer.failedLines());
        }
    });

    const bool ok = receiver.run();  // binds the socket, then blocks until SIGINT/SIGTERM

    reporting.store(false, std::memory_order_relaxed);
    reporter.join();

    if (!ok) {
        return 1;  // bind failed (bad address / port in use) — logged
    }

    // decode-dropped: bad size / unknown symbol. queue-dropped: shed by the
    // writer when QuestDB couldn't keep up and the buffer hit its cap.
    // post-failed: lines a POST couldn't persist (transport error / non-2xx).
    logLine("IngestCommand: shutting down (received={}, decode-dropped={}, "
            "queue-dropped={}, post-failed={})",
            received.load(), dropped.load(), writer.droppedLines(),
            writer.failedLines());
    return 0;
}
