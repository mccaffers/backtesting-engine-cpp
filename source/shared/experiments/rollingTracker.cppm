// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// rollingTracker — chainMatcher's trailing-window plumbing, on its own so the
// matcher file is state machine only. Trailing time-window extremes plus tick
// coverage, the rangeBarBuilder monotonic-deque pattern with timestamp expiry.
// Entries are valid while entry.ts + window >= ts (boundary inclusive,
// matching the anchored legs' tickTs - anchorTs <= WINDOW test).

module;

export module rollingTracker;

import std;  // replaces <chrono>, <deque>, <optional>

export namespace chain_matcher {

// One trailing window over a tick stream: feed every (mid, timestamp) pair
// in stream order; query the extremes, their pre-insert snapshots (NewExtreme's
// strict "beats every PRIOR tick in the lookback" test), and whether a full
// window of gap-free ticks has accumulated (the first tick ever fed counts as
// an infinite gap, so covered() also encodes warm-up).
class Tracker {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    explicit Tracker(const std::chrono::seconds window) : window_(window) {}

    // Expire entries (and gap records) older than the trailing window,
    // snapshot the pre-insert extremes, insert, and record the inter-tick
    // gap for the coverage measure.
    void feed(const TimePoint ts, const std::int32_t mid) {
        // Expire entries (and gap records) older than the trailing window.
        while (!maxDeque_.empty() && maxDeque_.front().ts + window_ < ts) {
            maxDeque_.pop_front();
        }
        while (!minDeque_.empty() && minDeque_.front().ts + window_ < ts) {
            minDeque_.pop_front();
        }
        while (!gapDeque_.empty() && gapDeque_.front().ts + window_ < ts) {
            gapDeque_.pop_front();
        }

        preMax_ = maxDeque_.empty() ? std::optional<std::int32_t>{}
                                    : maxDeque_.front().price;
        preMin_ = minDeque_.empty() ? std::optional<std::int32_t>{}
                                    : minDeque_.front().price;

        // Monotonic insert: popping equals keeps the newer entry, which
        // survives expiry longer (rangeBarBuilder's rule).
        while (!maxDeque_.empty() && maxDeque_.back().price <= mid) {
            maxDeque_.pop_back();
        }
        maxDeque_.push_back({ts, mid});
        while (!minDeque_.empty() && minDeque_.back().price >= mid) {
            minDeque_.pop_back();
        }
        minDeque_.push_back({ts, mid});

        // Coverage: track the largest inter-tick gap still inside the window
        // via its own monotonic max deque. The first tick ever fed counts as
        // an infinite gap, so covered() only turns true once a full window
        // of gap-free ticks has accumulated — warm-up and weekend gaps take
        // the same path.
        const auto gap = lastTick_ ? ts - *lastTick_
                                   : std::chrono::system_clock::duration::max();
        lastTick_ = ts;
        while (!gapDeque_.empty() && gapDeque_.back().gap <= gap) {
            gapDeque_.pop_back();
        }
        gapDeque_.push_back({ts, gap});
    }

    [[nodiscard]] bool hasData() const { return !maxDeque_.empty(); }
    [[nodiscard]] std::int32_t max() const { return maxDeque_.front().price; }
    [[nodiscard]] std::int32_t min() const { return minDeque_.front().price; }
    [[nodiscard]] std::optional<std::int32_t> preMax() const { return preMax_; }
    [[nodiscard]] std::optional<std::int32_t> preMin() const { return preMin_; }

    // A full window of real tick history: no gap as long as the window itself
    // remains inside it (the first-tick infinite gap covers warm-up).
    [[nodiscard]] bool covered() const {
        return !gapDeque_.empty() && gapDeque_.front().gap < window_;
    }

    void reset() {
        maxDeque_.clear();
        minDeque_.clear();
        gapDeque_.clear();
        lastTick_.reset();
        preMax_.reset();
        preMin_.reset();
    }

private:
    struct Entry {
        TimePoint ts;
        std::int32_t price;
    };
    struct GapEntry {
        TimePoint ts;
        std::chrono::system_clock::duration gap;
    };

    std::chrono::seconds window_;
    std::deque<Entry> maxDeque_;
    std::deque<Entry> minDeque_;
    std::deque<GapEntry> gapDeque_;
    std::optional<TimePoint> lastTick_;
    std::optional<std::int32_t> preMax_;
    std::optional<std::int32_t> preMin_;
};

}  // namespace chain_matcher
