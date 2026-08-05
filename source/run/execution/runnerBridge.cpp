// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/execution/runnerBridge.hpp"

#include "shared/utilities/env.hpp"

import std;
import priceData;       // PriceData
import tickCache;       // tick_cache::TickCache, SliceView, sliceForWindow
import backtestRunner;  // loadTickSuperset, loadTicks, runBacktestOnTicks
import rollingWindow;   // rolling::kFullHistory

// The tick window the opaque handle wraps. Kept out of redisRunner.cpp so that
// TU never has to name a module type or import a module — see runnerBridge.hpp
// for why that matters. The SliceView's shared_ptr keeps the whole superset
// alive while any task still holds a copy of this slice.
struct TickSliceImpl {
    tick_cache::SliceView view;
};

namespace {

// Positive integer env override, engine-fatal on junk: a mistyped cache knob
// should fail the worker at startup, not silently run with a default.
int envInt(const char* name, const std::string& fallback) {
    const std::string raw = env::getOr(name, fallback);
    int value = 0;
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || value < 0) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + raw);
    }
    return value;
}

// The worker's one cache instance. Function-local static so construction (env
// reads) happens on first use, after main() has the environment set up.
// Single-threaded by construction: bridgeGetTicks is only ever called from the
// one drain coroutine on the io_context thread; pool threads only read the
// immutable Supersets through their own shared_ptr copies, never the cache.
tick_cache::TickCache& cacheFor(const std::string& questdbHost) {
    static tick_cache::TickCache cache(
        // Superset loads capture the host by value — it is fixed per worker
        // process (argv), so one cache serves every run the worker claims.
        [host = questdbHost](const std::string& symbolsCsv, int months) {
            return loadTickSuperset(host, symbolsCsv, months);
        },
        // Ad-hoc loads (windows too deep for the superset) keep the original
        // now()-relative single-window behavior.
        [host = questdbHost](const std::string& symbolsCsv, int lastMonths,
                             int offsetMonths) {
            return loadTicks(host, symbolsCsv, lastMonths, offsetMonths);
        },
        std::chrono::minutes(envInt("TICK_CACHE_TTL_MINUTES", "60")),
        static_cast<std::size_t>(envInt("TICK_CACHE_MAX_SUPERSETS", "1")),
        // The ladder's deepest window is the terminal full-history run, so a
        // default superset serves every rung. Single source of truth for the
        // depth: rollingWindow's kFullHistory.
        rolling::kFullHistory.lastMonths);
    return cache;
}

}  // namespace

TickSlice bridgeGetTicks(const std::string& questdbHost,
                         const std::string& symbolsCsv,
                         const int lastMonths,
                         const int offsetMonths,
                         const std::function<void()>& beforeLoad) {
    auto impl = std::make_shared<TickSliceImpl>(TickSliceImpl{
        cacheFor(questdbHost).get(symbolsCsv, lastMonths, offsetMonths, beforeLoad)});
    const std::size_t count = impl->view.count;
    return TickSlice{std::move(impl), count};
}

void bridgeRunOnTicks(const TickSlice& slice,
                      const tradingDefinitions::Configuration& config) {
    const tick_cache::SliceView& view = slice.impl->view;
    const std::span<const PriceData> ticks =
        std::span(view.superset->ticks).subspan(view.begin, view.count);
    // The bridge is the Redis-queue path by construction (redisRunner is its
    // only consumer), so queue runs always participate in the rolling-window
    // ladder; direct `run <host> <config>` invocations bypass the bridge and
    // never chain.
    runBacktestOnTicks(ticks, config, /*chainWindows=*/true);
}
