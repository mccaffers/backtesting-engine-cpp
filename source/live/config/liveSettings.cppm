// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveSettings — the live subcommand's runtime configuration, resolved once
// at startup from the environment and argv. Every knob parses forgivingly
// (garbage falls back to the default) so a stray env var can't crash startup.

module;

#include "shared/net/udpPorts.hpp"
#include "shared/utilities/env.hpp"

export module liveSettings;

import std;

export namespace live {

// Defaults in parens; see Settings::fromEnv for the exact env names.
struct Settings {
    std::string bindAddr;          // LIVE_BIND_ADDR (127.0.0.1)
    std::uint16_t bindPort{};      // argv[2] > LIVE_UDP_PORT > kLive (11110)
    double minScore{};             // LIVE_MIN_SCORE floor on
                                   //   results.performanceScore (20)
    double maxDrawdownPercent{};   // LIVE_MAX_DRAWDOWN_PERCENT ceiling on
                                   //   results.maxDrawdownPercent (10) — a
                                   //   hard eligibility gate: the blended
                                   //   Calmar half of performanceScore lets
                                   //   a high-expectancy spiky run buy its
                                   //   way past the drawdown penalty
    double minCalmarScore{};       // LIVE_MIN_CALMAR_SCORE floor on
                                   //   results.calmarScore (30) — Calmar
                                   //   ratio ~2 on resultsSummary's scale
                                   //   (ratio 3 == 50): growth must be ~2x
                                   //   the worst giveback. Complements the
                                   //   ceiling, does not replace it — a
                                   //   fast-growing run can hold Calmar 2
                                   //   with a deep absolute drawdown, which
                                   //   the ceiling still refuses
    std::chrono::seconds lockTtl{};  // LIVE_TRADE_LOCK_SECONDS (30)
    std::string redisHost;         // REDIS_HOST (127.0.0.1)
    int redisPort{};               // fixed 6379, matching loadCommand
    std::size_t topPerGroup{};     // winners kept per (symbol, strategy): 3
    // TRADING_ENVIRONMENT ("live" | "demo", lowercased; default demo — the
    // safe account). Selects the IG session item the login service keeps in
    // DynamoDB: Auth#<tradingEnv>.
    std::string tradingEnv;
    // OHLC_PREPOPULATE (default on; "0" disables — same gate string, same
    // default, that ohlcBuilder itself re-reads at each symbol's first
    // tick). Surfaced here only so startup can log the bar warm-up mode —
    // the builder owns the behaviour, this field must never gate anything.
    bool ohlcPrepopulate{};

    static Settings fromEnv(int argc, const char* argv[]);
};

}  // namespace live

namespace {

// Parse a port from a string, returning `fallback` on empty/garbage input.
std::uint16_t parsePort(std::string_view text, const std::uint16_t fallback) {
    unsigned value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value == 0 || value > 65535) {
        return fallback;
    }
    return static_cast<std::uint16_t>(value);
}

// stod (not from_chars) matches how this codebase parses doubles from strings
// (see readIntField in tradingVariables.hpp).
double parseScore(const std::string& text, const double fallback) {
    try {
        const double value = std::stod(text);
        return std::isfinite(value) ? value : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

// Positive whole seconds or the fallback.
std::chrono::seconds parseTtlSeconds(std::string_view text, const long fallback) {
    long value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value <= 0) {
        return std::chrono::seconds{fallback};
    }
    return std::chrono::seconds{value};
}

}  // namespace

namespace live {

Settings Settings::fromEnv(const int argc, const char* argv[]) {
    Settings settings;
    settings.bindAddr = env::getOr("LIVE_BIND_ADDR", "127.0.0.1");

    // Bind port: CLI arg (argv[2]) overrides $LIVE_UDP_PORT overrides kLive.
    settings.bindPort =
        parsePort(env::getOr("LIVE_UDP_PORT", ""), udp_ports::kLive);
    if (argc >= 3) {
        settings.bindPort = parsePort(argv[2], settings.bindPort);
    }

    settings.minScore = parseScore(env::getOr("LIVE_MIN_SCORE", ""), 20.0);
    settings.maxDrawdownPercent =
        parseScore(env::getOr("LIVE_MAX_DRAWDOWN_PERCENT", ""), 10.0);
    settings.minCalmarScore =
        parseScore(env::getOr("LIVE_MIN_CALMAR_SCORE", ""), 30.0);
    settings.lockTtl =
        parseTtlSeconds(env::getOr("LIVE_TRADE_LOCK_SECONDS", ""), 30);
    settings.redisHost = env::getOr("REDIS_HOST", "127.0.0.1");
    settings.redisPort = 6379;
    settings.topPerGroup = 3;
    settings.ohlcPrepopulate = env::getOr("OHLC_PREPOPULATE", "1") == "1";
    settings.tradingEnv = env::getOr("TRADING_ENVIRONMENT", "demo");
    std::ranges::transform(settings.tradingEnv, settings.tradingEnv.begin(),
                           [](const unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
    return settings;
}

}  // namespace live
