// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// chainMatcher — the pure half of the analysis worker (trackingReport style):
// counts non-overlapping occurrences of an experiment's activity chain in a
// tick stream. Everything here is I/O-free — ticks arrive as (mid, timestamp)
// pairs, the return values are plain data — so the unit tests need no servers.
//
// Matcher semantics (pinned by tests/chainMatcher.cpp):
//
//  - Touch-within-window: a leg completes at the FIRST tick where its
//    condition holds with tickTs - anchorTs <= WINDOW_SECONDS. "Drops 1% in
//    10m" = touches -1% at any tick inside the window.
//  - Leg 1 is rolling, not anchored: tracked via trailing-window extremes
//    (monotonic max/min deques with TIMESTAMP-based expiry — the
//    rangeBarBuilder.cppm deque pattern adapted from tick-count to time
//    expiry; legitimate here because experiments are explicitly time-window
//    questions, and evaluation still advances only on tick timestamps — no
//    clock anywhere).
//  - Anchoring: leg N+1 anchors at leg N's completion tick (price = that
//    tick's mid, window from its timestamp); evaluation of leg N+1 starts on
//    the NEXT tick, so a chain can never telescope through one tick.
//  - Expiry: the first tick past the window fails the whole chain (the
//    condition can no longer be touched inside the window) — reset to
//    scanning-for-leg-1 and re-evaluate leg 1 on that same tick. Leg-1
//    trackers are fed every tick, so no re-scan is needed.
//  - Non-overlap: on completion, increment the count, clear ALL trackers,
//    resume next tick. Lookback legs re-warm after each match (documented
//    slight undercount).
//  - Serial attempts: a single anchor in flight; new leg-1 triggers are
//    picked up only after the current attempt fails (documented v1
//    undercount vs multi-anchor).
//  - Per-attempt recorder (phase 2): every attempt tracks its excursion
//    above/below the LEG-1 anchor mid from the anchor tick to resolution
//    (the resolving tick included, pre-anchor and post-resolution prices
//    excluded), the anchor->completion duration for completed attempts, and
//    the ask-bid spread at the anchor tick. Samples are exact up to
//    kMaxAttemptSamples per matcher, then sampling stops while counts stay
//    exact (samplesTruncated flags it). A single-leg chain's attempts
//    resolve on their own anchor tick, so their excursions/durations are
//    truthfully zero.
//  - Attempts/failures: every leg-1 completion counts an ATTEMPT (the
//    denominator for P(chain | leg 1) — for a single-leg chain attempts ==
//    occurrences by construction); every expiry counts a failure attributed
//    to the leg being sought (failuresByLeg; index 0 stays 0, leg 1 never
//    expires). A chain that fails and immediately re-anchors on the failing
//    tick counts BOTH the failure and the new attempt. Because attempts are
//    serial single-anchor, completion rates derived from them are
//    conservative in clustered/volatile periods (documented bias).
//  - Gaps/warm-up: band/lookback legs require actual tick coverage — the
//    tracker must have been fed for a full window with no inter-tick gap as
//    long as the window itself (tracked by a third monotonic deque of
//    inter-tick gaps; the first tick counts as an infinite gap, so coverage
//    also encodes warm-up). A weekend gap therefore cannot satisfy "stays in
//    band", and windows spanning gaps stay uncovered until a full fresh
//    window of ticks has accumulated.
//  - Arithmetic: mid = (int64(ask)+bid)/2; percent thresholds are
//    precomputed at config time as pctScaled = llround(pct * 1e6); every
//    per-tick test is pure int64, e.g.
//      (rollMax - cur) * 100'000'000 >= rollMax * pctScaled
//    (both sides of (rollMax-cur)/rollMax >= pct/100 multiplied out) — no
//    per-tick floating point.
//  - RangeRelativeMove basis: the trailing high-low mid range over
//    LOOKBACK_SECONDS (monotonic deques), NOT bar-based atr::calculate —
//    matches the rangeBarBuilder doctrine and keeps a bar builder out of the
//    matcher. Threshold = max(1, range x ATR_MULTIPLE) points; the JSON
//    field stays ATR_MULTIPLE. For anchored legs the threshold FREEZES at
//    the anchor tick (the volatility the setup completed under); as leg 1 it
//    is recomputed per tick, which is meaningful when LOOKBACK_SECONDS <
//    WINDOW_SECONDS (a move measured against a shorter recent-range basis) —
//    with ATR_MULTIPLE >= 1 and LOOKBACK >= WINDOW the basis always contains
//    the move itself, so the leg can never fire (documented v1 shape).

module;

#include "shared/experiments/experimentConfig.hpp"

export module chainMatcher;

import std;
import priceData;  // PriceData — evaluateExperiment's input stream

namespace {

using TimePoint = std::chrono::system_clock::time_point;
using Seconds = std::chrono::seconds;

// Trailing time-window extremes plus tick coverage, the rangeBarBuilder
// monotonic-deque pattern with timestamp expiry. Entries are valid while
// entry.ts + window >= ts (boundary inclusive, matching the anchored legs'
// tickTs - anchorTs <= WINDOW test).
class Tracker {
public:
    explicit Tracker(const Seconds window) : window_(window) {}

    // Feed one tick: expire, snapshot the pre-insert extremes (NewExtreme's
    // strict "beats every PRIOR tick in the lookback" test), insert, and
    // record the inter-tick gap for the coverage measure.
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
        // an infinite gap, so covered() only turns true once a full window of
        // gap-free ticks has accumulated — warm-up and weekend gaps take the
        // same path.
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

    Seconds window_;
    std::deque<Entry> maxDeque_;
    std::deque<Entry> minDeque_;
    std::deque<GapEntry> gapDeque_;
    std::optional<TimePoint> lastTick_;
    std::optional<std::int32_t> preMax_;
    std::optional<std::int32_t> preMin_;
};

}  // namespace

export namespace chain_matcher {

// Per-matcher cap on attempt SAMPLES (excursions/durations). Counts are
// never capped — beyond this, quantiles come from a truncated population and
// the outcome's samplesTruncated flag says so.
inline constexpr std::size_t kMaxAttemptSamples = 100'000;

// One resolved attempt's excursion record: max points above/below the leg-1
// anchor over the attempt's lifetime, plus the anchor itself so percent
// variants can be derived at aggregation time (FP once per attempt at most,
// never per tick).
struct ExcursionSample {
    std::int64_t abovePoints = 0;
    std::int64_t belowPoints = 0;
    std::int32_t anchorMid = 0;
};

struct MatchStats {
    std::uint64_t occurrences = 0;
    // Leg-1 completions, i.e. anchors created — the denominator for
    // P(chain | leg 1). Serial single-anchor, so conservative in clustered
    // periods (see the module comment). Equals occurrences for a
    // single-leg chain.
    std::uint64_t attempts = 0;
    // Expiry counts attributed to the leg being sought when the chain died.
    // Sized to the chain length; index 0 stays 0 (leg 1 never expires).
    std::vector<std::uint64_t> failuresByLeg;
    // Raw attempt samples, completed and failed kept separate (stop-loss vs
    // take-profit populations). completionSeconds is parallel to
    // completedExcursions (pushed under the same cap gate).
    std::vector<ExcursionSample> completedExcursions;
    std::vector<ExcursionSample> failedExcursions;
    std::vector<std::int64_t> completionSeconds;
    // Ask-bid spread at every attempt's anchor tick — mean only, so a plain
    // sum/count pair that never truncates.
    std::int64_t spreadAtTriggerSum = 0;
    std::uint64_t spreadAtTriggerCount = 0;
    bool samplesTruncated = false;
};

// One symbol's matcher: feed every tick, in stream order. Pure state machine —
// see the module comment for the pinned semantics.
class ChainMatcher {
public:
    // Validates the chain (throws std::invalid_argument on an empty chain or
    // a leg whose primitive is missing the fields it reads) and precomputes
    // every per-tick threshold as int64, so onTick never touches FP.
    // maxAttemptSamples overrides the sample cap — a test knob; production
    // callers take the default.
    explicit ChainMatcher(const experiments::ExperimentConfig& config,
                          const std::size_t maxAttemptSamples = kMaxAttemptSamples)
        : maxSamples_(maxAttemptSamples) {
        if (config.CHAIN.empty()) {
            throw std::invalid_argument("ChainMatcher: CHAIN must not be empty");
        }
        legs_.reserve(config.CHAIN.size());
        for (std::size_t i = 0; i < config.CHAIN.size(); ++i) {
            legs_.push_back(makeLeg(config.CHAIN[i], i));
        }
        stats_.failuresByLeg.assign(legs_.size(), 0);
    }

    void onTick(const std::int32_t mid, const std::int32_t spread,
                const TimePoint ts) {
        // All legs' rolling trackers are fed every tick (so a failed attempt
        // can re-evaluate leg 1 with no re-scan, and a chained leg's basis is
        // already warm when its turn comes); the state machine below consults
        // only the active leg.
        for (LegRuntime& leg : legs_) {
            if (leg.moveTracker) {
                leg.moveTracker->feed(ts, mid);
            }
            if (leg.basisTracker) {
                leg.basisTracker->feed(ts, mid);
            }
        }

        if (active_ > 0) {
            // The in-flight attempt's excursion vs its LEG-1 anchor — this
            // tick included, whether it advances, resolves, or expires the
            // attempt.
            const auto cur = static_cast<std::int64_t>(mid);
            attemptAbove_ = std::max(attemptAbove_, cur - attemptAnchorMid_);
            attemptBelow_ = std::max(attemptBelow_, attemptAnchorMid_ - cur);

            const LegRuntime& leg = legs_[active_];
            if (ts - anchorTs_ > leg.window) {
                // Expiry: the condition can no longer be touched inside the
                // window — the whole chain fails, attributed to the leg
                // being sought. Fall through to re-evaluate leg 1 on this
                // same tick (which may itself anchor a fresh attempt).
                recordAttempt(/*completed=*/false, ts);
                ++stats_.failuresByLeg[active_];
                active_ = 0;
            } else if (conditionHolds(active_, mid)) {
                completeActiveLeg(mid, spread, ts);
                return;
            } else {
                return;
            }
        }

        // Scanning for leg 1 (rolling, no anchor).
        if (conditionHolds(0, mid)) {
            completeActiveLeg(mid, spread, ts);
        }
    }

    [[nodiscard]] const MatchStats& stats() const { return stats_; }

    // 0-based index of the leg currently being sought — for tests.
    [[nodiscard]] std::size_t activeLeg() const { return active_; }

private:
    struct LegRuntime {
        experiments::ActivityType type;
        Seconds window{0};              // completion window (anchored legs)
        std::int64_t pctScaled = 0;     // llround(|MOVE_PERCENT| * 1e6)
        bool negativeMove = false;      // DirectionalMove: drop vs rise
        int direction = 0;              // NewExtreme / RangeRelativeMove
        std::int64_t atrScaled = 0;     // llround(ATR_MULTIPLE * 1e6)
        // Rolling extremes over WINDOW_SECONDS — leg 1's anchor substitute
        // (DirectionalMove / RangeRelativeMove in first position only).
        std::optional<Tracker> moveTracker;
        // Trailing basis over the band/lookback window (StaysInBand,
        // NewExtreme, RangeRelativeMove — any position).
        std::optional<Tracker> basisTracker;
    };

    static LegRuntime makeLeg(const experiments::Activity& activity,
                              const std::size_t index) {
        using experiments::ActivityType;
        LegRuntime leg{.type = activity.TYPE,
                       .window = Seconds(activity.WINDOW_SECONDS)};
        const auto requirePositive = [&](const int value, const char* field) {
            if (value < 1) {
                throw std::invalid_argument(
                    std::format("ChainMatcher: leg {} ({}) requires {} >= 1",
                                index + 1, experiments::toString(activity.TYPE),
                                field));
            }
        };
        const auto requireDirection = [&] {
            if (activity.DIRECTION != 1 && activity.DIRECTION != -1) {
                throw std::invalid_argument(std::format(
                    "ChainMatcher: leg {} ({}) requires DIRECTION of +1 or -1",
                    index + 1, experiments::toString(activity.TYPE)));
            }
            leg.direction = activity.DIRECTION;
        };

        switch (activity.TYPE) {
            case ActivityType::DirectionalMove: {
                if (activity.MOVE_PERCENT == 0.0) {
                    throw std::invalid_argument(std::format(
                        "ChainMatcher: leg {} (DirectionalMove) requires a "
                        "non-zero MOVE_PERCENT",
                        index + 1));
                }
                requirePositive(activity.WINDOW_SECONDS, "WINDOW_SECONDS");
                leg.negativeMove = activity.MOVE_PERCENT < 0.0;
                leg.pctScaled =
                    std::llround(std::abs(activity.MOVE_PERCENT) * 1e6);
                if (index == 0) {
                    leg.moveTracker.emplace(leg.window);
                }
                break;
            }
            case ActivityType::StaysInBand: {
                if (activity.MOVE_PERCENT == 0.0) {
                    throw std::invalid_argument(std::format(
                        "ChainMatcher: leg {} (StaysInBand) requires a "
                        "non-zero MOVE_PERCENT (band width)",
                        index + 1));
                }
                // The band is measured over LOOKBACK_SECONDS when set,
                // falling back to WINDOW_SECONDS — so a leg-1 band needs no
                // separate deadline field.
                const int bandSeconds = activity.LOOKBACK_SECONDS > 0
                                            ? activity.LOOKBACK_SECONDS
                                            : activity.WINDOW_SECONDS;
                requirePositive(bandSeconds, "LOOKBACK_SECONDS (or WINDOW_SECONDS)");
                if (index > 0) {
                    requirePositive(activity.WINDOW_SECONDS, "WINDOW_SECONDS");
                }
                leg.pctScaled =
                    std::llround(std::abs(activity.MOVE_PERCENT) * 1e6);
                leg.basisTracker.emplace(Seconds(bandSeconds));
                break;
            }
            case ActivityType::NewExtreme: {
                requirePositive(activity.LOOKBACK_SECONDS, "LOOKBACK_SECONDS");
                requireDirection();
                if (index > 0) {
                    requirePositive(activity.WINDOW_SECONDS, "WINDOW_SECONDS");
                }
                leg.basisTracker.emplace(Seconds(activity.LOOKBACK_SECONDS));
                break;
            }
            case ActivityType::RangeRelativeMove: {
                requirePositive(activity.LOOKBACK_SECONDS, "LOOKBACK_SECONDS");
                requirePositive(activity.WINDOW_SECONDS, "WINDOW_SECONDS");
                requireDirection();
                if (activity.ATR_MULTIPLE <= 0.0) {
                    throw std::invalid_argument(std::format(
                        "ChainMatcher: leg {} (RangeRelativeMove) requires "
                        "ATR_MULTIPLE > 0",
                        index + 1));
                }
                leg.atrScaled = std::llround(activity.ATR_MULTIPLE * 1e6);
                leg.basisTracker.emplace(Seconds(activity.LOOKBACK_SECONDS));
                if (index == 0) {
                    leg.moveTracker.emplace(leg.window);
                }
                break;
            }
        }
        return leg;
    }

    // max(1, range x ATR_MULTIPLE) in points — the >= 1 clamp stops a
    // dead-flat basis (threshold 0) from firing on a zero move.
    [[nodiscard]] static std::int64_t rangeThreshold(const std::int64_t range,
                                                     const std::int64_t atrScaled) {
        return std::max<std::int64_t>(1,
                                      (range * atrScaled + 500'000) / 1'000'000);
    }

    // Does leg `index`'s condition hold at the current tick? Leg 1 (index 0)
    // measures against its rolling trackers; anchored legs against
    // anchorMid_. All int64, no per-tick FP.
    [[nodiscard]] bool conditionHolds(const std::size_t index,
                                      const std::int32_t mid) const {
        using experiments::ActivityType;
        const LegRuntime& leg = legs_[index];
        const auto cur = static_cast<std::int64_t>(mid);

        switch (leg.type) {
            case ActivityType::DirectionalMove: {
                if (index == 0) {
                    const Tracker& roll = *leg.moveTracker;
                    if (!roll.hasData()) {
                        return false;
                    }
                    // (extreme - cur) / extreme >= pct/100, multiplied out.
                    if (leg.negativeMove) {
                        const auto rollMax = static_cast<std::int64_t>(roll.max());
                        return (rollMax - cur) * 100'000'000 >=
                               rollMax * leg.pctScaled;
                    }
                    const auto rollMin = static_cast<std::int64_t>(roll.min());
                    return (cur - rollMin) * 100'000'000 >=
                           rollMin * leg.pctScaled;
                }
                const auto anchor = static_cast<std::int64_t>(anchorMid_);
                if (leg.negativeMove) {
                    return (anchor - cur) * 100'000'000 >= anchor * leg.pctScaled;
                }
                return (cur - anchor) * 100'000'000 >= anchor * leg.pctScaled;
            }
            case ActivityType::StaysInBand: {
                const Tracker& basis = *leg.basisTracker;
                if (!basis.covered()) {
                    return false;  // a gap/warm-up window can't satisfy a band
                }
                const auto range = static_cast<std::int64_t>(basis.max()) -
                                   basis.min();
                return range * 100'000'000 <= cur * leg.pctScaled;
            }
            case ActivityType::NewExtreme: {
                const Tracker& basis = *leg.basisTracker;
                if (!basis.covered()) {
                    return false;  // no fire before a full lookback of ticks
                }
                // Strict: the current mid must beat every PRIOR tick in the
                // lookback (pre-insert snapshot), not merely equal it.
                if (leg.direction > 0) {
                    return basis.preMax() && cur > *basis.preMax();
                }
                return basis.preMin() && cur < *basis.preMin();
            }
            case ActivityType::RangeRelativeMove: {
                const Tracker& basis = *leg.basisTracker;
                if (index == 0) {
                    if (!basis.covered()) {
                        return false;
                    }
                    const auto range = static_cast<std::int64_t>(basis.max()) -
                                       basis.min();
                    const std::int64_t threshold =
                        rangeThreshold(range, leg.atrScaled);
                    const Tracker& roll = *leg.moveTracker;
                    if (leg.direction < 0) {
                        return static_cast<std::int64_t>(roll.max()) - cur >=
                               threshold;
                    }
                    return cur - static_cast<std::int64_t>(roll.min()) >=
                           threshold;
                }
                // Anchored: threshold frozen at the anchor tick (see
                // completeActiveLeg); invalid when the basis was uncovered at
                // anchor time — the leg then never fires and the chain
                // expires naturally.
                if (!anchorThresholdValid_) {
                    return false;
                }
                const auto anchor = static_cast<std::int64_t>(anchorMid_);
                if (leg.direction < 0) {
                    return anchor - cur >= anchorThreshold_;
                }
                return cur - anchor >= anchorThreshold_;
            }
        }
        return false;
    }

    // The resolved attempt's samples, under the shared cap gate (counts are
    // never capped — only samples). completionSeconds stays parallel to
    // completedExcursions because both are pushed here together.
    void recordAttempt(const bool completed, const TimePoint ts) {
        std::vector<ExcursionSample>& samples =
            completed ? stats_.completedExcursions : stats_.failedExcursions;
        if (samples.size() >= maxSamples_) {
            stats_.samplesTruncated = true;
            return;
        }
        samples.push_back({attemptAbove_, attemptBelow_,
                           static_cast<std::int32_t>(attemptAnchorMid_)});
        if (completed) {
            stats_.completionSeconds.push_back(
                std::chrono::duration_cast<std::chrono::seconds>(
                    ts - attemptAnchorTs_)
                    .count());
        }
    }

    // The active leg's condition held at this tick: either the chain is done
    // (count it, clear everything, resume next tick) or the next leg anchors
    // here — and, since evaluation returns to the caller, it is first
    // evaluated on the NEXT tick, never its own anchor tick.
    void completeActiveLeg(const std::int32_t mid, const std::int32_t spread,
                           const TimePoint ts) {
        if (active_ == 0) {
            ++stats_.attempts;  // a leg-1 completion IS an attempt
            // Arm the attempt recorder at the leg-1 anchor: excursions are
            // measured from HERE for the whole chain (anchorMid_ moves at
            // every later leg; this baseline must not), and the spread is
            // sampled at this tick only.
            attemptAnchorMid_ = mid;
            attemptAnchorTs_ = ts;
            attemptAbove_ = 0;
            attemptBelow_ = 0;
            stats_.spreadAtTriggerSum += spread;
            ++stats_.spreadAtTriggerCount;
        }
        if (active_ + 1 == legs_.size()) {
            ++stats_.occurrences;
            recordAttempt(/*completed=*/true, ts);
            resetAfterMatch();
            return;
        }
        ++active_;
        anchorMid_ = mid;
        anchorTs_ = ts;

        // RangeRelativeMove anchored legs freeze their threshold from the
        // basis range as it stood at the anchor — the volatility the setup
        // completed under, not whatever the move itself later inflates it to.
        const LegRuntime& next = legs_[active_];
        if (next.type == experiments::ActivityType::RangeRelativeMove) {
            const Tracker& basis = *next.basisTracker;
            anchorThresholdValid_ = basis.covered();
            anchorThreshold_ =
                anchorThresholdValid_
                    ? rangeThreshold(static_cast<std::int64_t>(basis.max()) -
                                         basis.min(),
                                     next.atrScaled)
                    : 0;
        }
    }

    // Non-overlap: after a full match, every tracker is cleared — a pre-match
    // extreme can never seed the next occurrence, and lookback legs re-warm
    // (documented slight undercount).
    void resetAfterMatch() {
        active_ = 0;
        for (LegRuntime& leg : legs_) {
            if (leg.moveTracker) {
                leg.moveTracker->reset();
            }
            if (leg.basisTracker) {
                leg.basisTracker->reset();
            }
        }
    }

    std::vector<LegRuntime> legs_;
    MatchStats stats_;
    std::size_t maxSamples_ = kMaxAttemptSamples;
    std::size_t active_ = 0;         // index of the leg currently sought
    std::int32_t anchorMid_ = 0;     // active leg's anchor (legs > 0)
    TimePoint anchorTs_{};
    std::int64_t anchorThreshold_ = 0;      // frozen RangeRelativeMove threshold
    bool anchorThresholdValid_ = false;
    // The in-flight attempt's recorder state (leg-1 anchor baseline).
    std::int64_t attemptAnchorMid_ = 0;
    TimePoint attemptAnchorTs_{};
    std::int64_t attemptAbove_ = 0;
    std::int64_t attemptBelow_ = 0;
};

// Aggregation helpers for evaluateExperiment — FP is fine here, this runs
// once per experiment after the scan, never per tick.
namespace detail {

// Nearest-rank percentile over an already-sorted, non-empty vector.
inline double nearestRank(const std::vector<double>& sorted, const double q) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(q * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(rank, 1) - 1];
}

inline experiments::QuantilePair quantiles(std::vector<double>& values) {
    std::ranges::sort(values);
    return {nearestRank(values, 0.5), nearestRank(values, 0.9)};
}

// Which excursion side counts as FAVORABLE (MFE), from the final leg's
// implied direction. StaysInBand has none — "up" is the documented default.
inline bool favorableUp(const experiments::Activity& finalLeg) {
    switch (finalLeg.TYPE) {
        case experiments::ActivityType::DirectionalMove:
            return finalLeg.MOVE_PERCENT >= 0.0;
        case experiments::ActivityType::NewExtreme:
        case experiments::ActivityType::RangeRelativeMove:
            return finalLeg.DIRECTION >= 0;
        case experiments::ActivityType::StaysInBand:
            return true;
    }
    return true;
}

// Summarise one attempt population; nullopt when empty (the doc then reads
// null, never fake zeros).
inline std::optional<experiments::ExcursionStats> summariseExcursions(
    const std::vector<ExcursionSample>& samples, const bool up) {
    if (samples.empty()) {
        return std::nullopt;
    }
    std::vector<double> mfePoints, maePoints, mfePercent, maePercent;
    mfePoints.reserve(samples.size());
    maePoints.reserve(samples.size());
    mfePercent.reserve(samples.size());
    maePercent.reserve(samples.size());
    for (const ExcursionSample& s : samples) {
        const auto favorable =
            static_cast<double>(up ? s.abovePoints : s.belowPoints);
        const auto adverse =
            static_cast<double>(up ? s.belowPoints : s.abovePoints);
        mfePoints.push_back(favorable);
        maePoints.push_back(adverse);
        mfePercent.push_back(100.0 * favorable / s.anchorMid);
        maePercent.push_back(100.0 * adverse / s.anchorMid);
    }
    return experiments::ExcursionStats{
        .samples = samples.size(),
        .mfePoints = quantiles(mfePoints),
        .maePoints = quantiles(maePoints),
        .mfePercent = quantiles(mfePercent),
        .maePercent = quantiles(maePercent),
    };
}

}  // namespace detail

// Replays one experiment over a (possibly multi-symbol, UNION-ALL ordered)
// tick stream: the stream is demuxed into one ChainMatcher per symbol, so
// each symbol's chain is counted independently and the aggregate sums them.
// Throws (std::invalid_argument) on an invalid chain — the worker's
// per-experiment poison-pill path.
experiments::ExperimentOutcome evaluateExperiment(
    const std::span<const PriceData> ticks,
    const experiments::ExperimentConfig& config) {
    // Validate up front (even for an empty stream) so a malformed experiment
    // is retired loudly by the worker, never reported as "0 occurrences".
    [[maybe_unused]] const ChainMatcher validation(config);

    std::map<std::string, ChainMatcher> matchers;

    // Per-symbol tick counts and spans, tracked alongside the matchers so a
    // sparsely ticked symbol's rate is judged against its OWN coverage, not
    // the shared stream's.
    struct SymbolSpan {
        std::uint64_t ticks = 0;
        TimePoint minTs = TimePoint::max();
        TimePoint maxTs = TimePoint::min();
    };
    std::map<std::string, SymbolSpan> spans;

    experiments::ExperimentOutcome outcome;
    outcome.ticksScanned = ticks.size();
    outcome.failuresByLeg.assign(config.CHAIN.size(), 0);

    TimePoint minTs = TimePoint::max();
    TimePoint maxTs = TimePoint::min();
    for (const PriceData& tick : ticks) {
        const auto mid = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(tick.ask) + tick.bid) / 2);
        ChainMatcher& matcher =
            matchers.try_emplace(tick.symbol, config).first->second;
        // Delta-detection: a matcher completes at most one occurrence per
        // tick, so an increment here identifies the completing tick — the
        // calendar bucketing lives HERE, keeping the matcher core free of
        // it.
        const std::uint64_t before = matcher.stats().occurrences;
        matcher.onTick(mid, tick.ask - tick.bid, tick.timestamp);
        if (matcher.stats().occurrences > before) {
            const auto day =
                std::chrono::floor<std::chrono::days>(tick.timestamp);
            const std::chrono::year_month_day ymd{day};
            ++outcome.occurrencesByMonth[std::format(
                "{:04}-{:02}", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()))];
            const auto hour = std::chrono::duration_cast<std::chrono::hours>(
                                  tick.timestamp - day)
                                  .count();
            ++outcome.occurrencesByHourUtc[static_cast<std::size_t>(hour)];
        }

        SymbolSpan& span = spans[tick.symbol];
        ++span.ticks;
        span.minTs = std::min(span.minTs, tick.timestamp);
        span.maxTs = std::max(span.maxTs, tick.timestamp);
        minTs = std::min(minTs, tick.timestamp);
        maxTs = std::max(maxTs, tick.timestamp);
    }

    if (!ticks.empty()) {
        outcome.daysSpanned =
            std::chrono::duration<double, std::ratio<86'400>>(maxTs - minTs)
                .count();
    }
    // Merged attempt samples across the demuxed matchers (bounded by the
    // per-matcher cap x symbol count).
    std::vector<ExcursionSample> completed;
    std::vector<ExcursionSample> failed;
    std::vector<double> completionSeconds;
    std::int64_t spreadSum = 0;
    std::uint64_t spreadCount = 0;
    for (const auto& [symbol, matcher] : matchers) {
        const MatchStats& stats = matcher.stats();
        outcome.occurrences += stats.occurrences;
        outcome.attempts += stats.attempts;
        for (std::size_t leg = 0; leg < stats.failuresByLeg.size(); ++leg) {
            outcome.failuresByLeg[leg] += stats.failuresByLeg[leg];
        }
        const SymbolSpan& span = spans.at(symbol);
        outcome.perSymbol[symbol] = experiments::SymbolOutcome{
            .occurrences = stats.occurrences,
            .ticksScanned = span.ticks,
            .daysSpanned =
                std::chrono::duration<double, std::ratio<86'400>>(span.maxTs -
                                                                  span.minTs)
                    .count(),
        };
        outcome.occurrencesBySymbol[symbol] = stats.occurrences;  // v1 shape

        completed.insert(completed.end(), stats.completedExcursions.begin(),
                         stats.completedExcursions.end());
        failed.insert(failed.end(), stats.failedExcursions.begin(),
                      stats.failedExcursions.end());
        for (const std::int64_t seconds : stats.completionSeconds) {
            completionSeconds.push_back(static_cast<double>(seconds));
        }
        spreadSum += stats.spreadAtTriggerSum;
        spreadCount += stats.spreadAtTriggerCount;
        outcome.samplesTruncated =
            outcome.samplesTruncated || stats.samplesTruncated;
    }

    const bool up = detail::favorableUp(config.CHAIN.back());
    outcome.excursionOrientation = up ? "up" : "down";
    outcome.completedAttempts = detail::summariseExcursions(completed, up);
    outcome.failedAttempts = detail::summariseExcursions(failed, up);
    if (!completionSeconds.empty()) {
        outcome.completionSeconds = detail::quantiles(completionSeconds);
    }
    if (spreadCount > 0) {
        outcome.meanSpreadAtTriggerPoints =
            static_cast<double>(spreadSum) / static_cast<double>(spreadCount);
    }
    return outcome;
}

}  // namespace chain_matcher
