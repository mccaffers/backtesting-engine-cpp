// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// rangeBarBuilder — incremental tick -> range bar aggregation.
//
// A range bar completes when price travels a threshold distance within the
// bar (high - low >= threshold, ask prices). The breaching tick is included
// in the bar and closes it at its REAL price — overshoot past the threshold
// is kept, and the NEXT tick opens the new bar at its real price. No
// synthetic boundary closes and no phantom gap bars: same real-prices-only
// doctrine as ohlcBuilder (which deliberately dropped the C# "join up"
// close overwrite). One tick therefore closes at most one bar.
//
// The threshold is dynamic and PURELY event-driven: a rolling high-low range
// over the last RANGE_ATR_TICK_WINDOW ticks, tracked by two monotonic deques
// (O(1) amortised per tick). There is deliberately no clock anywhere in the
// construct — an earlier design that grouped ticks into fixed-duration
// true-range slices was rejected because a volatility spike early in a slice
// would not move the measure until the slice rolled; the tick-count window
// widens on the very tick a spike happens and forgets it exactly
// RANGE_ATR_TICK_WINDOW ticks later. Session/weekend gaps need no special
// handling: a gap inside the window is captured by max - min automatically.
//
// The threshold locks when a bar opens (a bar's target never moves under it)
// and no bar exists until the window has seen a full RANGE_ATR_TICK_WINDOW of
// ticks — so every bar's threshold is locked from a fully-warm measure, and
// consumers' existing "short series = not warm" gates cover the start-up.
//
// Pre-population mirrors ohlcBuilder's: on a series' very first update the
// builder fetches raw ticks ending strictly before that tick from QuestDB and
// replays them through the same state machine — one replay warms both the
// window and the bar history, in backtest (seeded from before the replay
// window, so nothing is double-counted and there is no lookahead) and live
// (first tick on the worker thread) alike. Range bars cannot be aggregated
// server-side (SAMPLE BY is time-bucketed), but the pure tick-count measure
// makes the fetch exact instead of heuristic: RANGE_ATR_TICK_WINDOW rows warm
// the window perfectly by definition, plus a bounded margin for bar
// formation. Gated by the same OHLC_PREPOPULATE switch (default ON) so sweeps
// that silence warm-up DB traffic silence this too.

module;

#include "shared/utilities/env.hpp"

export module rangeBarBuilder;

import std;                 // replaces <chrono>, <cstdint>, <deque>, <vector>
import connectionFactory;   // questdb::connectionFromEnv
import databaseConnection;  // DatabaseConnection::executeQuery
import ohlcObject;          // the bar record being built (shared with OHLC)
import priceData;           // PriceData tick
import symbolScale;         // symbol whitelist guard before SQL interpolation

export namespace rangebar {

// One range-bar series' shape. Identity is (atrTickWindow, atrPercent) —
// count is only the window depth kept (and the prepopulate depth), so
// registering the same identity twice merges to the larger count, mirroring
// BarStore::registerSeries.
struct RangeBarSpec {
    int atrTickWindow;  // rolling tick window for the range measure
    int atrPercent;     // threshold = windowRange * atrPercent / 100
    int count;          // bars kept AND the prepopulate margin driver
};

[[nodiscard]] constexpr bool sameIdentity(const RangeBarSpec& a,
                                          const RangeBarSpec& b) {
    return a.atrTickWindow == b.atrTickWindow && a.atrPercent == b.atrPercent;
}

// Rows fetched by the warm-up query. The window term is exact — that many
// ticks warm the measure perfectly, by definition of a tick-count window.
// The margin covers bar formation, which IS an estimate: over any window of
// atrTickWindow ticks price spans windowRange, and a bar needs atrPercent% of
// that, so ~atrTickWindow * pct / 100 ticks form one bar on average; x2 for
// safety. Partial BAR warm-up is accepted by design (strategies gate on
// series size) — the most recent rows come back first, so the window (the
// part that must be right) always warms. The overall clamp bounds the
// transient PriceData vector (~1M rows ≈ one peak FX day ≈ 50 MB, freed
// after replay).
inline constexpr std::int64_t kMaxPrepopulateTicks = 1'000'000;

[[nodiscard]] constexpr std::int64_t prepopulateTickCap(const RangeBarSpec& spec) {
    const std::int64_t ticksPerBar =
        (static_cast<std::int64_t>(spec.atrTickWindow) * spec.atrPercent + 99) / 100;
    const std::int64_t margin = 2 * static_cast<std::int64_t>(spec.count) * ticksPerBar;
    return std::min(spec.atrTickWindow + margin, kMaxPrepopulateTicks);
}

// SQL for the warm-up ticks, ending strictly before `before` (the first
// tick's timestamp — live: "now"; backtest: keeps every replay tick out of
// the seed, so nothing is double-counted and there is no lookahead). The
// SELECT shape matches sqlManager's tick queries so
// DatabaseConnection::executeQuery parses the rows as-is. No time lower
// bound: the row count is what matters for a tick-count measure, and LIMIT
// bounds the scan (QuestDB walks the designated timestamp index backwards).
std::string prepopulateTicksQuery(std::string_view symbol,
                                  std::chrono::system_clock::time_point before,
                                  const RangeBarSpec& spec) {
    const std::int64_t beforeMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(before.time_since_epoch())
            .count();
    return std::format(
        "SELECT '{}' as symbol, ask, bid, timestamp FROM '{}' "
        "WHERE timestamp < cast({}L AS timestamp) "
        "ORDER BY timestamp DESC LIMIT {}",
        symbol, symbol, beforeMicros, prepopulateTickCap(spec));
}

// Fetches the warm-up ticks ending just before `before`, oldest first, ready
// to replay through the series. Returns empty — a plain cold start — when the
// gate is off, the symbol is unknown (also the SQL-injection guard: symbols
// are interpolated, not bound, same as sqlManager), or anything DB-side
// fails. One attempt, no retries.
std::vector<PriceData> prepopulateTicks(std::string_view symbol,
                                        std::chrono::system_clock::time_point before,
                                        const RangeBarSpec& spec) {
    if (spec.atrTickWindow < 1 || spec.atrPercent < 1 || spec.count < 1) {
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
        std::vector<PriceData> ticks =
            db.executeQuery(prepopulateTicksQuery(symbol, before, spec));
        std::ranges::reverse(ticks);
        return ticks;
    } catch (const std::exception& e) {
        std::println(std::cerr, "prepopulateTicks({}): {} — cold start", symbol,
                     e.what());
        return {};
    }
}

// One symbol's range-bar series: the rolling tick window plus the bar
// history. Bars reuse OhlcObject so atr/ema/swingPivots and strategy code
// consume them exactly like time bars. The vector keeps ohlcBuilder's
// "last element in-progress" invariant with one nuance: completion is known
// AT the breaching tick, so the last element may be just-closed
// (complete == true) until the next tick pushes its successor — marking it
// lazily would delay truthful information for no benefit.
//
// NOT thread-safe: owned via BarStore, one instance per backtest run / live
// worker.
class RangeSeries {
public:
    explicit RangeSeries(const RangeBarSpec& spec) : spec_(spec) {
        // A zero window can never warm, a zero percent would floor every
        // threshold, a zero count can hold no bars — die loudly at setup,
        // not per tick (same rationale as calculateOHLC's duration throw).
        if (spec.atrTickWindow < 1 || spec.atrPercent < 1 || spec.count < 1) {
            throw std::invalid_argument(
                "RangeSeries: all RangeBarSpec fields must be >= 1");
        }
    }

    // Feed one tick, exactly once, in stream order. The very first call
    // attempts the QuestDB warm-up and replays it through the same state
    // machine before applying the live tick; the flag is set before the
    // query so a DB failure is a cold start, never a retry storm.
    void update(const PriceData& tick) {
        if (!prepopulateAttempted_) {
            prepopulateAttempted_ = true;
            for (const PriceData& seed :
                 prepopulateTicks(tick.symbol, tick.timestamp, spec_)) {
                applyTick(seed);
            }
        }
        applyTick(tick);
    }

    // Chronological; see the class comment for the completion nuance.
    [[nodiscard]] const std::vector<OhlcObject>& bars() const { return bars_; }

    [[nodiscard]] const RangeBarSpec& spec() const { return spec_; }

    // The window has seen a full atrTickWindow of ticks; bars only form from
    // here on. Exposed (with the two below) for tests and diagnostics.
    [[nodiscard]] bool warm() const {
        return tickIndex_ >= static_cast<std::uint64_t>(spec_.atrTickWindow);
    }

    // Rolling high - low over the last atrTickWindow ticks (fewer while
    // warming). 0 before any tick.
    [[nodiscard]] std::int32_t windowRange() const {
        if (maxDeque_.empty()) {
            return 0;
        }
        return maxDeque_.front().price - minDeque_.front().price;
    }

    // The in-progress bar's locked threshold; 0 before the first bar opens.
    [[nodiscard]] std::int32_t lockedThreshold() const { return lockedThreshold_; }

private:
    struct Entry {
        std::uint64_t idx;
        std::int32_t price;
    };

    // The pure state machine — replayed warm-up ticks and live ticks take
    // exactly this path, so the two cannot diverge. Order matters: the
    // window advances FIRST, so a bar opening on tick T locks a threshold
    // that already includes T's own contribution (the same "the tick's own
    // price counts" doctrine as the run loop's bars-before-decide ordering).
    void applyTick(const PriceData& tick) {
        const std::int32_t price = tick.ask;
        ++tickIndex_;

        // (1) Monotonic deques: max non-increasing, min non-decreasing.
        // Popping equals keeps the newer index, which survives expiry longer.
        while (!maxDeque_.empty() && maxDeque_.back().price <= price) {
            maxDeque_.pop_back();
        }
        maxDeque_.push_back({tickIndex_, price});
        while (!minDeque_.empty() && minDeque_.back().price >= price) {
            minDeque_.pop_back();
        }
        minDeque_.push_back({tickIndex_, price});
        // Expire entries that fell out of the last-atrTickWindow window:
        // valid indices are (tickIndex - window, tickIndex].
        const auto window = static_cast<std::uint64_t>(spec_.atrTickWindow);
        while (maxDeque_.front().idx + window <= tickIndex_) {
            maxDeque_.pop_front();
        }
        while (minDeque_.front().idx + window <= tickIndex_) {
            minDeque_.pop_front();
        }

        // (2) Bar logic. Pre-warm ticks feed only the window — no bar exists
        // yet, so every bar's threshold locks from a fully-warm measure.
        if (!warm()) {
            return;
        }
        if (bars_.empty() || bars_.back().complete) {
            // Open at this tick and lock its threshold NOW. A fresh bar can
            // never be born complete: its range is 0 and the floor keeps the
            // threshold >= 1 point even when windowRange * pct rounds to 0
            // (the flat-market degenerate case) — so a dead-flat stream holds
            // one in-progress bar instead of rolling a bar per tick.
            const std::int64_t scaled =
                (static_cast<std::int64_t>(windowRange()) * spec_.atrPercent + 50) /
                100;
            lockedThreshold_ =
                static_cast<std::int32_t>(std::max<std::int64_t>(1, scaled));
            bars_.push_back({.date = tick.timestamp,
                             .open = price,
                             .close = price,
                             .high = price,
                             .low = price});
        } else {
            OhlcObject& bar = bars_.back();
            if (price > bar.high) {
                bar.high = price;
            }
            if (price < bar.low) {
                bar.low = price;
            }
            bar.close = price;
            if (bar.high - bar.low >= lockedThreshold_) {
                // Breaching tick included: the close is this real price and
                // any overshoot past the threshold is kept. The next tick
                // opens the successor, so a gap crossing ten thresholds
                // still closes exactly one bar.
                bar.complete = true;
            }
        }

        // (3) Rolling window of bars: oldest dropped from the front. Never
        // touches the deques — the measure and the history age independently.
        const auto cap = static_cast<std::size_t>(spec_.count);
        if (bars_.size() > cap) {
            bars_.erase(bars_.begin(),
                        bars_.end() - static_cast<std::ptrdiff_t>(cap));
        }
    }

    RangeBarSpec spec_;
    std::vector<OhlcObject> bars_;
    std::deque<Entry> maxDeque_;
    std::deque<Entry> minDeque_;
    std::uint64_t tickIndex_ = 0;  // 1-based after the first tick
    std::int32_t lockedThreshold_ = 0;
    bool prepopulateAttempted_ = false;
};

}  // namespace rangebar
