// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module tickCache;

import std;        // replaces <chrono>, <cstddef>, <functional>, <memory>,
                   // <string>, <unordered_map>, <vector>
import priceData;  // PriceData

// Cross-run tick reuse for the queue worker. The rolling-window ladder turns
// every surviving strategy into its own single-strategy run, so without a
// cache each offset window pays a full QuestDB load per strategy. But the
// ladder's windows all nest inside one months-deep history ending at a common
// snapshot, and ticks are time-ordered — so one superset load serves every
// window as a contiguous slice found by binary search.
export namespace tick_cache {

// One loaded block of tick history for a symbols set: the ticks span
// [boundaries[months], boundaries[0]) where boundaries[m] is QuestDB's own
// dateadd('M', -m, now()) at load time (index 0 = the snapshot instant T).
// Boundaries travel with the ticks so slicing can never disagree with the
// query that produced them. Immutable once published — pool tasks read it
// concurrently through shared_ptr copies.
struct Superset {
    std::vector<PriceData> ticks;
    std::vector<std::chrono::system_clock::time_point> boundaries;
};

// A window of a superset: ticks[begin, begin+count). Holds the superset alive,
// so a task can outlive the cache entry it was sliced from.
struct SliceView {
    std::shared_ptr<const Superset> superset;
    std::size_t begin = 0;
    std::size_t count = 0;
};

// The window (lastMonths, offsetMonths) as a slice of `superset`:
// [boundaries[last+offset], boundaries[offset]), matching the SQL semantics
// `timestamp >= lower AND timestamp < upper`. lower_bound gives the first tick
// >= a boundary, which is simultaneously the inclusive start at the lower
// bound and the exclusive end at the upper. offset == 0 keeps the upper end
// open (the superset's own end), mirroring loadPriceData leaving the upper
// bound off entirely for unshifted windows.
// Throws std::logic_error when the window reaches deeper than the superset —
// callers are expected to have checked fit and fallen back to an ad-hoc load.
SliceView sliceForWindow(std::shared_ptr<const Superset> superset,
                         int lastMonths,
                         int offsetMonths) {
    if (lastMonths < 1 || offsetMonths < 0) {
        throw std::logic_error("sliceForWindow: degenerate window");
    }
    const auto depth = static_cast<std::size_t>(lastMonths) +
                       static_cast<std::size_t>(offsetMonths);
    if (depth >= superset->boundaries.size()) {
        throw std::logic_error("sliceForWindow: window reaches beyond the superset");
    }

    const auto byTimestamp = [](const PriceData& tick,
                                const std::chrono::system_clock::time_point boundary) {
        return tick.timestamp < boundary;
    };
    const auto& ticks = superset->ticks;
    const auto lower = std::lower_bound(ticks.begin(), ticks.end(),
                                        superset->boundaries[depth], byTimestamp);
    const auto upper = offsetMonths == 0
        ? ticks.end()
        : std::lower_bound(lower, ticks.end(),
                           superset->boundaries[static_cast<std::size_t>(offsetMonths)],
                           byTimestamp);

    return SliceView{
        .superset = std::move(superset),
        .begin = static_cast<std::size_t>(lower - ticks.begin()),
        .count = static_cast<std::size_t>(upper - lower),
    };
}

// Keyed superset cache with TTL and size-capped eviction. Loading is injected
// (SupersetLoader for cacheable superset pulls, WindowLoader for one-off
// windows too deep to fit) so the machinery tests without QuestDB; NowFn is
// injected so TTL expiry tests without sleeping.
//
// NOT thread-safe by design: the one drain coroutine is the only caller.
// Pool threads never touch the cache — they only hold shared_ptr copies of
// the immutable Supersets it hands out.
class TickCache {
public:
    using SupersetLoader = std::function<Superset(const std::string& symbolsCsv, int months)>;
    using WindowLoader = std::function<std::vector<PriceData>(const std::string& symbolsCsv,
                                                              int lastMonths,
                                                              int offsetMonths)>;
    using NowFn = std::function<std::chrono::steady_clock::time_point()>;

    TickCache(SupersetLoader supersetLoader,
              WindowLoader windowLoader,
              std::chrono::seconds ttl,
              std::size_t maxSupersets,
              int defaultMonths,
              NowFn now = [] { return std::chrono::steady_clock::now(); })
        : supersetLoader_(std::move(supersetLoader)),
          windowLoader_(std::move(windowLoader)),
          ttl_(ttl),
          maxSupersets_(std::max<std::size_t>(1, maxSupersets)),
          defaultMonths_(defaultMonths),
          now_(std::move(now)) {}

    // The ticks for one run's window. beforeLoad() is invoked exactly once,
    // immediately before ANY QuestDB load (superset miss, TTL reload, or
    // ad-hoc window) and never on a cache hit — the drain loop quiesces its
    // pool there so no in-flight backtest still reads a buffer being evicted.
    SliceView get(const std::string& symbolsCsv,
                  const int lastMonths,
                  const int offsetMonths,
                  const std::function<void()>& beforeLoad = {}) {
        const auto callBeforeLoad = [&] {
            if (beforeLoad) {
                beforeLoad();
            }
        };

        // Degenerate windows go straight to the ad-hoc loader (current
        // now()-relative behavior), untracked. A window deeper than a FRESH
        // resident superset is also served ad hoc — a hand-queued one-off must
        // not evict the ladder's resident superset — but on a plain miss the
        // new superset simply grows to fit (max below).
        const bool cacheable = lastMonths >= 1 && offsetMonths >= 0;
        const int monthsNeeded = cacheable ? lastMonths + offsetMonths : 0;

        if (cacheable) {
            if (const auto it = supersets_.find(symbolsCsv); it != supersets_.end()) {
                const Entry& entry = it->second;
                const bool fresh = now_() - entry.loadedAt < ttl_;
                const bool fits =
                    static_cast<std::size_t>(monthsNeeded) < entry.superset->boundaries.size();
                if (fresh && fits) {
                    ++hits_;
                    auto slice = sliceForWindow(entry.superset, lastMonths, offsetMonths);
                    std::println("TickCache: HIT symbols={} window=({},{}) ticks={} [{}..{}) hits={} misses={}",
                                 symbolsCsv, lastMonths, offsetMonths, slice.count,
                                 slice.begin, slice.begin + slice.count, hits_, misses_);
                    return slice;
                }
                if (fresh && !fits) {
                    // The resident superset is good, just not deep enough for
                    // this window — load the window ad hoc and keep the entry.
                    return loadAdHoc(symbolsCsv, lastMonths, offsetMonths, callBeforeLoad);
                }
                // Stale: fall through to reload below (evicting this entry).
            }

            ++misses_;
            callBeforeLoad();
            // Evict BEFORE loading: beforeLoad's quiesce guarantees nothing
            // still reads the old buffers, and dropping them first keeps the
            // peak at ~one superset plus the load's own parse spike.
            evictFor(symbolsCsv);
            const int months = std::max(defaultMonths_, monthsNeeded);
            const auto loadStart = std::chrono::steady_clock::now();
            auto superset = std::make_shared<const Superset>(
                supersetLoader_(symbolsCsv, months));
            const std::chrono::duration<double> loadSeconds =
                std::chrono::steady_clock::now() - loadStart;
            std::println("TickCache: superset loaded symbols={} months={} ticks={} in {:.1f}s hits={} misses={}",
                         symbolsCsv, months, superset->ticks.size(),
                         loadSeconds.count(), hits_, misses_);
            supersets_.insert_or_assign(symbolsCsv, Entry{superset, now_()});
            return sliceForWindow(std::move(superset), lastMonths, offsetMonths);
        }

        return loadAdHoc(symbolsCsv, lastMonths, offsetMonths, callBeforeLoad);
    }

private:
    struct Entry {
        std::shared_ptr<const Superset> superset;
        std::chrono::steady_clock::time_point loadedAt;
    };

    // A one-off load outside the superset scheme, wrapped as a whole-buffer
    // slice with no boundaries (it is never re-sliced or cached).
    SliceView loadAdHoc(const std::string& symbolsCsv,
                        const int lastMonths,
                        const int offsetMonths,
                        const std::function<void()>& callBeforeLoad) {
        callBeforeLoad();
        std::println("TickCache: ad-hoc load symbols={} window=({},{})",
                     symbolsCsv, lastMonths, offsetMonths);
        auto superset = std::make_shared<const Superset>(
            Superset{windowLoader_(symbolsCsv, lastMonths, offsetMonths), {}});
        const std::size_t count = superset->ticks.size();
        return SliceView{std::move(superset), 0, count};
    }

    // Drop stale entries and, if the incoming key still pushes the cache over
    // its cap, the oldest-loaded entries — the ladder drains rung by rung, so
    // the least recently loaded key is also the least likely to recur next.
    void evictFor(const std::string& incomingKey) {
        std::erase_if(supersets_, [&](const auto& kv) {
            return now_() - kv.second.loadedAt >= ttl_;
        });
        while (supersets_.size() >= maxSupersets_ &&
               !supersets_.contains(incomingKey)) {
            const auto oldest = std::min_element(
                supersets_.begin(), supersets_.end(), [](const auto& a, const auto& b) {
                    return a.second.loadedAt < b.second.loadedAt;
                });
            std::println("TickCache: evicting symbols={}", oldest->first);
            supersets_.erase(oldest);
        }
    }

    SupersetLoader supersetLoader_;
    WindowLoader windowLoader_;
    std::chrono::seconds ttl_;
    std::size_t maxSupersets_;
    int defaultMonths_;
    NowFn now_;
    std::unordered_map<std::string, Entry> supersets_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

}  // namespace tick_cache
