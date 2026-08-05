// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// ohlcBuilder — incremental tick -> OHLC bar aggregation.
//
// Port of the C# CalculateOHLC(PriceObj, decimal price, TimeSpan duration,
// List<OhlcObject>) helper. Called once per tick, it appends to / updates a
// caller-owned vector of bars: the last element is always the in-progress bar,
// everything before it is complete. Shared so both the backtester (run) and a
// future live path can build bars the same way.
//
// Type mapping from the C# original:
//  - decimal price          -> std::int32_t scaled points (engine convention;
//                              the caller picks ask/bid/mid, exactly as the C#
//                              passed price separately from priceObj)
//  - TimeSpan duration      -> std::chrono::system_clock::duration
//                              (std::chrono::minutes etc. convert implicitly)
//  - List<OhlcObject>       -> std::vector<OhlcObject>&, mutated in place
//                              (the C# returned the same list it mutated)
//
// Pre-population (port of the C# GetOHLCData): on the very first tick the bar
// list can be seeded with historical bars from QuestDB (SAMPLE BY over the
// symbol's tick table), so the strategy hits the ground running instead of
// waiting OHLC_COUNT * OHLC_MINUTES of ticks — backtests trade from the first
// replayed tick, live warms up on launch. ON by default; OHLC_PREPOPULATE=0
// is the kill switch for sweeps, which fire two queries per run per symbol —
// memoize per (symbol, minutes, count) if that ever hurts. Unit tests force
// the gate off at startup (tests/ohlc.cpp) to stay hermetic.
// Connection comes from QUESTDB_HOST/QUESTDB_PORT;
// the run command takes its QuestDB host via argv, so set the env var too when
// the host isn't local (live has no host argument — the env vars are its only
// source, and liveCommand logs the warm-up mode at startup). Bars are
// aggregated from ask only, matching the one
// consumer (the breakout strategy feeds tick.ask). Both paths now use true
// last-tick closes; the remaining accepted drift vs incrementally built bars:
// buckets are calendar-aligned rather than first-tick-anchored, and buckets
// with < 10 ticks are dropped.

module;

#include "shared/utilities/env.hpp"

export module ohlcBuilder;

import std;                 // replaces <chrono>, <cstdint>, <vector>
import connectionFactory;   // questdb::connectionFromEnv
import databaseConnection;  // DatabaseConnection::queryOhlc
import ohlcObject;          // the bar record being built
import priceData;           // PriceData tick (timestamp source)
import symbolScale;         // symbol whitelist guard before SQL interpolation

export namespace ohlc {

// SQL for the warm-up bars, ending strictly before `before` (the first tick's
// timestamp — for live that is "now", for a backtest it keeps every replay
// tick out of the seed, so nothing is double-counted and there is no
// lookahead). The bucket containing `before` comes back partial: exactly the
// in-progress bar a continuously running builder would hold.
//
// Unlike the C# original there is no LIMIT over-fetch multiplier — the
// ticks >= 10 filter runs server-side (QuestDB's no-HAVING idiom: aggregate in
// a subquery, filter outside) so LIMIT count is exact — and no per-symbol
// lookback table: one generous formula covers the worst case in
// symbol_scale::kTable (a ~6h/day index across a weekend, (24/6)*(7/5) = 5.6x
// calendar/trading -> 6x), plus 10 days for holiday clusters. Over-scanning is
// cheap; LIMIT caps the rows returned.
std::string prepopulateQuery(std::string_view symbol,
                             std::chrono::system_clock::time_point before,
                             std::chrono::minutes barMinutes, int count) {
    const std::int64_t beforeMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(before.time_since_epoch())
            .count();
    const std::int64_t totalMinutes = static_cast<std::int64_t>(barMinutes.count()) * count;
    const std::int64_t days = (totalMinutes + 1439) / 1440 * 6 + 10;
    const std::int64_t fromMicros = beforeMicros - days * 86'400'000'000;

    return std::format(
        "SELECT timestamp, open, high, low, close FROM ("
        "SELECT timestamp, first(ask) AS open, max(ask) AS high, min(ask) AS low, "
        "last(ask) AS close, count() AS ticks FROM '{}' "
        "WHERE timestamp >= cast({}L AS timestamp) AND timestamp < cast({}L AS timestamp) "
        "SAMPLE BY {}m ALIGN TO CALENDAR"
        ") WHERE ticks >= 10 ORDER BY timestamp DESC LIMIT {}",
        symbol, fromMicros, beforeMicros, barMinutes.count(), count);
}

// Fetches up to `count` warm-up bars ending just before `before`, oldest
// first, restoring the builder invariant (last element = in-progress bar).
// Returns empty — a plain cold start — when the gate is off, the symbol is
// unknown (also the SQL-injection guard: symbols are interpolated, not bound,
// same as sqlManager), or anything DB-side fails. One attempt, no retries.
std::vector<OhlcObject> prepopulateOHLC(std::string_view symbol,
                                        std::chrono::system_clock::time_point before,
                                        std::chrono::minutes barMinutes, int count) {
    if (count <= 0 || barMinutes < std::chrono::minutes{1}) {
        return {};
    }
    if (env::getOr("OHLC_PREPOPULATE", "1") != "1") {
        return {};
    }
    if (symbol_scale::get(symbol) == symbol_scale::kUnknown) {
        return {};
    }
    try {
        const DatabaseConnection db = questdb::connectionFromEnv();
        std::vector<OhlcObject> bars =
            db.queryOhlc(prepopulateQuery(symbol, before, barMinutes, count));
        std::ranges::reverse(bars);
        if (!bars.empty()) {
            bars.back().complete = false;
        }
        return bars;
    } catch (const std::exception& e) {
        std::println(std::cerr, "prepopulateOHLC({}): {} — cold start", symbol, e.what());
        return {};
    }
}

void calculateOHLC(const PriceData& tick, std::int32_t price,
                   std::chrono::system_clock::duration duration,
                   std::vector<OhlcObject>& bars, int prepopulateCount = 0) {
    // A non-positive duration would make every tick roll a new bar: one bar
    // per tick over a months-long tick stream grows the vector without bound
    // (OOM), and every "bar" is a single tick. Nothing upstream hard-validates
    // OHLC_MINUTES ({0,0} is a legal "no bars" sentinel for strategies that
    // never call this), so the strategy that DOES build bars must be stopped
    // here — the throw surfaces as a contained per-strategy failure.
    if (duration <= std::chrono::system_clock::duration::zero()) {
        throw std::invalid_argument(
            "calculateOHLC: bar duration must be positive (check OHLC_MINUTES)");
    }

    // First tick ever: seed from QuestDB history when enabled, else from the
    // tick. Either way bars is non-empty afterwards, so the query fires at
    // most once per bar list.
    if (bars.empty()) {
        if (prepopulateCount > 0) {
            bars = prepopulateOHLC(tick.symbol, tick.timestamp,
                                   std::chrono::duration_cast<std::chrono::minutes>(duration),
                                   prepopulateCount);
        }
        if (bars.empty()) {
            bars.push_back({.date = tick.timestamp,
                            .open = price,
                            .close = price,
                            .high = price,
                            .low = price});
        }
    }

    // Strictly greater-than, matching the C# `diff > duration.TotalMinutes`:
    // a tick landing exactly on the boundary still belongs to the open bar.
    if (tick.timestamp - bars.back().date > duration) {
        // The finished bar keeps the close it accumulated tick by tick — its
        // own bucket's true last price. (The C# original overwrote it with the
        // NEXT bucket's first price to make bars "join up", which could push
        // close outside [low, high] on a gap and disagreed with the
        // prepopulate query's last(ask) convention.)
        bars.back().complete = true;

        bars.push_back({.date = tick.timestamp,
                        .open = price,
                        .close = price,
                        .high = price,
                        .low = price});
    }

    OhlcObject& bar = bars.back();
    if (price > bar.high) {
        bar.high = price;
    }
    if (price < bar.low) {
        bar.low = price;
    }
    bar.close = price;
}

}  // namespace ohlc
