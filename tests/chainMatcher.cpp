// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// Pins the matcher semantics stated in chainMatcher.cppm's module comment:
// touch-within-window completion, rolling (not anchored) leg 1, next-tick
// anchoring, whole-chain expiry with same-tick re-evaluation, non-overlap
// with full tracker clears, and the coverage rules for band/lookback legs.
// Synthetic mid/timestamp sequences only — no servers.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "shared/experiments/experimentConfig.hpp"

import chainMatcher;
import priceData;

using chain_matcher::ChainMatcher;
using experiments::Activity;
using experiments::ActivityType;
using experiments::ExperimentConfig;

namespace {

// All timestamps are offsets (in seconds) from an arbitrary fixed epoch —
// the matcher only ever compares timestamps, never reads a clock.
std::chrono::system_clock::time_point at(const long seconds) {
    const std::chrono::system_clock::time_point base{
        std::chrono::sys_days{std::chrono::year{2026} / std::chrono::January / 5}};
    return base + std::chrono::seconds(seconds);
}

ExperimentConfig chainOf(std::vector<Activity> chain) {
    return ExperimentConfig{
        .UUID = "test-uuid", .NAME = "test", .CHAIN = std::move(chain)};
}

Activity move(const double signedPercent, const int windowSeconds) {
    return Activity{.TYPE = ActivityType::DirectionalMove,
                    .MOVE_PERCENT = signedPercent,
                    .WINDOW_SECONDS = windowSeconds};
}

}  // namespace

TEST_CASE("DirectionalMove fires on a touch within the trailing window",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99600, 0, at(60));
    CHECK(matcher.stats().occurrences == 0);
    // Exactly -1% from the rolling max, well inside the 10-minute window.
    matcher.onTick(99000, 0, at(120));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("DirectionalMove does not fire when the drop is slower than the window",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99500, 0, at(550));
    // The 100000 print expired from the trailing 600s window (deque expiry),
    // so the rolling max is 99500 and the remaining drop is only 0.5%.
    matcher.onTick(99000, 0, at(1101));
    CHECK(matcher.stats().occurrences == 0);
}

TEST_CASE("split drop still fires via rolling extremes (anti-re-anchor)",
          "[chainMatcher]") {
    // 0.6% then a further 0.5% inside one window: an anchored design that
    // re-anchored at the first partial drop would miss this; the rolling max
    // sees the cumulative 1.1%.
    ChainMatcher matcher(chainOf({move(-1.0, 600)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99400, 0, at(100));
    CHECK(matcher.stats().occurrences == 0);
    matcher.onTick(98900, 0, at(200));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("two-leg chain anchors at leg 1's completion tick", "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600), move(0.5, 600)}));
    matcher.onTick(100000, 0, at(0));
    CHECK(matcher.activeLeg() == 0);
    matcher.onTick(99000, 0, at(60));  // leg 1 completes: anchor (99000, 60)
    CHECK(matcher.activeLeg() == 1);
    CHECK(matcher.stats().occurrences == 0);
    // +0.5% from the ANCHOR (99000 -> 99495), inside leg 2's own window.
    matcher.onTick(99495, 0, at(120));
    CHECK(matcher.stats().occurrences == 1);
    CHECK(matcher.activeLeg() == 0);
}

TEST_CASE("a leg cannot complete on its own anchor tick", "[chainMatcher]") {
    // Leg 2 is a trailing band whose condition ALREADY holds on the tick
    // that completes leg 1 — but evaluation of leg N+1 starts on the NEXT
    // tick, so the chain needs one more tick to finish.
    ChainMatcher matcher(chainOf({
        move(-1.0, 600),
        Activity{.TYPE = ActivityType::StaysInBand,
                 .MOVE_PERCENT = 5.0,
                 .WINDOW_SECONDS = 600,
                 .LOOKBACK_SECONDS = 300},
    }));
    for (long t = 0; t <= 300; t += 30) {
        matcher.onTick(100000, 0, at(t));
    }
    CHECK(matcher.activeLeg() == 0);
    matcher.onTick(99000, 0, at(330));  // -1%: leg 1 completes here
    CHECK(matcher.activeLeg() == 1);
    // The band (range 1000 <= 5% of mid, fully covered) held at the anchor
    // tick too — but only the NEXT tick may complete the chain.
    CHECK(matcher.stats().occurrences == 0);
    matcher.onTick(99000, 0, at(360));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("an expired attempt counts a failure against the leg being sought",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600), move(0.5, 60)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99000, 0, at(100));  // leg 1 completes: attempt 1 anchors
    CHECK(matcher.stats().attempts == 1);
    // 61s > leg 2's window, and this tick is only -0.6% off the rolling max —
    // the chain dies (attributed to leg 2) WITHOUT re-anchoring.
    matcher.onTick(99400, 0, at(161));
    CHECK(matcher.activeLeg() == 0);
    CHECK(matcher.stats().attempts == 1);
    CHECK(matcher.stats().occurrences == 0);
    REQUIRE(matcher.stats().failuresByLeg.size() == 2);
    CHECK(matcher.stats().failuresByLeg[0] == 0);  // leg 1 never expires
    CHECK(matcher.stats().failuresByLeg[1] == 1);
}

TEST_CASE("mid-chain expiry fails the chain and the failing tick can start a new attempt",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600), move(0.5, 60)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99000, 0, at(100));  // leg 1 completes: anchor (99000, 100)
    CHECK(matcher.activeLeg() == 1);
    // 61s > leg 2's 60s window: the attempt fails — and this same tick is a
    // fresh -2% off the still-live rolling max, so leg 1 completes AGAIN on
    // the failing tick (trackers were fed all along, no re-scan).
    matcher.onTick(98000, 0, at(161));
    CHECK(matcher.stats().occurrences == 0);
    CHECK(matcher.activeLeg() == 1);
    // The failing tick counted BOTH the failure and the fresh attempt.
    CHECK(matcher.stats().attempts == 2);
    CHECK(matcher.stats().failuresByLeg[1] == 1);
    // The new attempt is anchored at (98000, 161): +0.5% completes it.
    matcher.onTick(98490, 0, at(200));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("non-overlap: back-to-back matches count, pre-match extremes cannot seed",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99000, 0, at(60));
    CHECK(matcher.stats().occurrences == 1);  // first match clears ALL trackers
    // 1.5% below the PRE-match max — but that extreme is gone; the fresh
    // tracker's max is this tick itself, so nothing fires.
    matcher.onTick(98500, 0, at(120));
    CHECK(matcher.stats().occurrences == 1);
    // A full -1% against the POST-match max (98500 -> 98515 needed; 985
    // points is exactly 1%): back-to-back occurrence number two.
    matcher.onTick(97515, 0, at(180));
    CHECK(matcher.stats().occurrences == 2);
    // Single-leg chains: every leg-1 completion IS the whole chain, so
    // attempts == occurrences by construction (and nothing can expire).
    CHECK(matcher.stats().attempts == 2);
    CHECK(matcher.stats().failuresByLeg == std::vector<std::uint64_t>{0});
}

TEST_CASE("serial attempts: an interleaved second trigger yields one occurrence",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600), move(0.5, 600)}));
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99000, 0, at(60));   // attempt 1 anchors at 99000
    CHECK(matcher.activeLeg() == 1);
    // A further drop that would qualify as a NEW leg-1 trigger — ignored,
    // a single anchor is in flight (documented v1 undercount).
    matcher.onTick(98000, 0, at(120));
    CHECK(matcher.activeLeg() == 1);
    // Recovery to +0.5% off the FIRST anchor completes exactly one chain.
    matcher.onTick(99495, 0, at(240));
    CHECK(matcher.stats().occurrences == 1);
    CHECK(matcher.activeLeg() == 0);
}

TEST_CASE("StaysInBand requires a full covered window before it can fire",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({Activity{.TYPE = ActivityType::StaysInBand,
                                           .MOVE_PERCENT = 0.5,
                                           .LOOKBACK_SECONDS = 300}}));
    // Dead flat from the first tick — but the band cannot fire until a full
    // 300s of real tick history has accumulated (warm-up is coverage too).
    for (long t = 0; t <= 300; t += 30) {
        matcher.onTick(100000, 0, at(t));
        CHECK(matcher.stats().occurrences == 0);
    }
    matcher.onTick(100000, 0, at(330));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("StaysInBand: a gap voids coverage until a fresh window accumulates",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({Activity{.TYPE = ActivityType::StaysInBand,
                                           .MOVE_PERCENT = 5.0,
                                           .LOOKBACK_SECONDS = 300}}));
    // Oscillate outside the band (range ~5.8% > 5%) so nothing fires while
    // the tracker is warm and covered...
    for (long t = 0; t <= 600; t += 30) {
        matcher.onTick(t % 60 == 0 ? 100000 : 106000, 0, at(t));
    }
    CHECK(matcher.stats().occurrences == 0);
    // ...then a 400s silent gap (> the 300s window). The post-gap window
    // looks dead flat — the oscillating prints all expired — but a weekend
    // gap must NOT satisfy "stays in band": coverage is void until 300s of
    // dense post-gap ticks accumulate.
    for (long t = 1000; t <= 1300; t += 30) {
        matcher.onTick(100000, 0, at(t));
        CHECK(matcher.stats().occurrences == 0);
    }
    matcher.onTick(100000, 0, at(1330));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("NewExtreme fires on a strict new high, never before coverage",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({Activity{.TYPE = ActivityType::NewExtreme,
                                           .LOOKBACK_SECONDS = 300,
                                           .DIRECTION = 1}}));
    // Steadily rising: every tick beats every prior print, but nothing may
    // fire until the lookback is fully covered.
    for (long t = 0; t <= 300; t += 30) {
        matcher.onTick(static_cast<std::int32_t>(100000 + t / 3), 0, at(t));
        CHECK(matcher.stats().occurrences == 0);
    }
    matcher.onTick(100110, 0, at(330));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("NewExtreme is strict: equalling the lookback high does not fire",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({Activity{.TYPE = ActivityType::NewExtreme,
                                           .LOOKBACK_SECONDS = 300,
                                           .DIRECTION = 1}}));
    // Dead flat, fully covered: the current mid always EQUALS the trailing
    // high — a new high must strictly exceed it.
    for (long t = 0; t <= 600; t += 30) {
        matcher.onTick(100000, 0, at(t));
    }
    CHECK(matcher.stats().occurrences == 0);
    matcher.onTick(100001, 0, at(630));  // strictly above: fires
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("RangeRelativeMove clamps the threshold to >= 1 point", "[chainMatcher]") {
    ChainMatcher matcher(chainOf({Activity{.TYPE = ActivityType::RangeRelativeMove,
                                           .WINDOW_SECONDS = 600,
                                           .LOOKBACK_SECONDS = 300,
                                           .DIRECTION = -1,
                                           .ATR_MULTIPLE = 0.3}}));
    // Dead flat and covered: the basis range is 0, so an unclamped threshold
    // would be 0 and a zero move would "fire" on every tick. The >= 1 clamp
    // keeps a flat market silent.
    for (long t = 0; t <= 600; t += 30) {
        matcher.onTick(100000, 0, at(t));
        CHECK(matcher.stats().occurrences == 0);
    }
    // A 1-point drop: the basis range grows to 1, 1 x 0.3 rounds to 0, the
    // clamp lifts it back to 1 point — met exactly by this move.
    matcher.onTick(99999, 0, at(630));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("anchored RangeRelativeMove freezes its threshold at the anchor",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({
        move(-1.0, 600),
        Activity{.TYPE = ActivityType::RangeRelativeMove,
                 .WINDOW_SECONDS = 600,
                 .LOOKBACK_SECONDS = 300,
                 .DIRECTION = -1,
                 .ATR_MULTIPLE = 2.0},
    }));
    // Warm the basis flat at 101000, then a -1.02% drop completes leg 1.
    // Basis range at the anchor = 101000 - 99980 = 1020, so leg 2's frozen
    // threshold is 2040 points below the anchor (99980).
    for (long t = 0; t <= 330; t += 30) {
        matcher.onTick(101000, 0, at(t));
    }
    matcher.onTick(99980, 0, at(360));
    CHECK(matcher.activeLeg() == 1);
    // 2020 points below the anchor: 20 short of the frozen threshold.
    matcher.onTick(97960, 0, at(420));
    CHECK(matcher.stats().occurrences == 0);
    // 2040 points below the anchor fires. Had the threshold been recomputed
    // live, the drop itself would have widened the basis range to ~3060
    // (threshold 6120) and this could never fire — freezing is the contract.
    matcher.onTick(97940, 0, at(480));
    CHECK(matcher.stats().occurrences == 1);
}

TEST_CASE("ChainMatcher validates the chain at construction", "[chainMatcher]") {
    CHECK_THROWS_AS(ChainMatcher(chainOf({})), std::invalid_argument);
    // DirectionalMove without a move.
    CHECK_THROWS_AS(ChainMatcher(chainOf({move(0.0, 600)})),
                    std::invalid_argument);
    // DirectionalMove without a window.
    CHECK_THROWS_AS(ChainMatcher(chainOf({move(-1.0, 0)})),
                    std::invalid_argument);
    // NewExtreme needs an explicit +1/-1 direction.
    CHECK_THROWS_AS(ChainMatcher(chainOf({Activity{
                        .TYPE = ActivityType::NewExtreme,
                        .LOOKBACK_SECONDS = 300}})),
                    std::invalid_argument);
    // RangeRelativeMove needs a positive multiplier.
    CHECK_THROWS_AS(ChainMatcher(chainOf({Activity{
                        .TYPE = ActivityType::RangeRelativeMove,
                        .WINDOW_SECONDS = 600,
                        .LOOKBACK_SECONDS = 300,
                        .DIRECTION = -1,
                        .ATR_MULTIPLE = 0.0}})),
                    std::invalid_argument);
}

TEST_CASE("evaluateExperiment demuxes symbols and reports the scan span",
          "[chainMatcher]") {
    const auto tick = [](const std::int32_t mid, const long seconds,
                         const char* symbol) {
        return PriceData(mid, mid, at(seconds), symbol);
    };
    // EURUSD dips a full 1% (one occurrence); GBPUSD only 0.1% (none). The
    // interleaved UNION-ALL order must not leak one symbol's prints into the
    // other's matcher, and the final tick stretches the span to 2 days.
    const std::vector<PriceData> ticks{
        tick(100000, 0, "EURUSD"),
        tick(100000, 30, "GBPUSD"),
        tick(99000, 60, "EURUSD"),
        tick(99900, 90, "GBPUSD"),
        tick(99000, 2 * 86400, "EURUSD"),
    };

    const auto outcome = chain_matcher::evaluateExperiment(
        ticks, chainOf({move(-1.0, 600)}));

    CHECK(outcome.occurrences == 1);
    CHECK(outcome.ticksScanned == 5);
    CHECK(outcome.daysSpanned == 2.0);
    REQUIRE(outcome.occurrencesBySymbol.size() == 2);
    CHECK(outcome.occurrencesBySymbol.at("EURUSD") == 1);
    CHECK(outcome.occurrencesBySymbol.at("GBPUSD") == 0);
}

TEST_CASE("evaluateExperiment validates the chain even for an empty stream",
          "[chainMatcher]") {
    CHECK_THROWS_AS(
        chain_matcher::evaluateExperiment({}, chainOf({move(0.0, 600)})),
        std::invalid_argument);
    // A VALID chain over an empty stream still reports a leg-sized failure
    // vector and zero attempts — the doc's completionRate then reads null,
    // never a fake 0.
    const auto empty =
        chain_matcher::evaluateExperiment({}, chainOf({move(-1.0, 600)}));
    CHECK(empty.attempts == 0);
    CHECK(empty.failuresByLeg == std::vector<std::uint64_t>{0});
}

TEST_CASE("evaluateExperiment buckets completions by UTC month and hour",
          "[chainMatcher]") {
    const auto tick = [](const std::int32_t mid, const long seconds) {
        return PriceData(mid, mid, at(seconds), "EURUSD");
    };
    // Occurrence 1 completes at the epoch tick + 60s: 2026-01-05, hour 0.
    // Occurrence 2 completes 27 days later at 13:00 UTC: 2026-02-01, hour 13
    // — straddling the month boundary splits the buckets.
    const long feb = 27 * 86400 + 13 * 3600;
    const std::vector<PriceData> ticks{
        tick(100000, 0),
        tick(99000, 60),        // completes: 2026-01, hour 0
        tick(99000, feb),       // fresh post-match trackers
        tick(98010, feb + 60),  // completes: 2026-02, hour 13
    };

    const auto outcome = chain_matcher::evaluateExperiment(
        ticks, chainOf({move(-1.0, 600)}));

    CHECK(outcome.occurrences == 2);
    REQUIRE(outcome.occurrencesByMonth.size() == 2);  // absent months absent
    CHECK(outcome.occurrencesByMonth.at("2026-01") == 1);
    CHECK(outcome.occurrencesByMonth.at("2026-02") == 1);
    CHECK(outcome.occurrencesByHourUtc[0] == 1);
    CHECK(outcome.occurrencesByHourUtc[13] == 1);
    std::uint64_t total = 0;
    for (const std::uint64_t count : outcome.occurrencesByHourUtc) {
        total += count;
    }
    CHECK(total == outcome.occurrences);
}

TEST_CASE("the attempt recorder tracks excursions from anchor to resolution only",
          "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600), move(0.5, 600)}));
    // COMPLETED attempt: anchor at (99000, 60); dips 200 below, completes
    // 495 above at t=180 (duration 120s). The pre-anchor 100000 print must
    // not register as excursion.
    matcher.onTick(100000, 0, at(0));
    matcher.onTick(99000, 0, at(60));
    matcher.onTick(98800, 0, at(120));
    matcher.onTick(99495, 0, at(180));
    CHECK(matcher.stats().occurrences == 1);
    REQUIRE(matcher.stats().completedExcursions.size() == 1);
    CHECK(matcher.stats().completedExcursions[0].abovePoints == 495);
    CHECK(matcher.stats().completedExcursions[0].belowPoints == 200);
    CHECK(matcher.stats().completedExcursions[0].anchorMid == 99000);
    REQUIRE(matcher.stats().completionSeconds.size() == 1);
    CHECK(matcher.stats().completionSeconds[0] == 120);

    // Post-resolution tick: trackers were cleared, nothing may extend the
    // recorded sample or start an attempt on its own.
    matcher.onTick(97000, 0, at(240));
    CHECK(matcher.stats().attempts == 1);

    // FAILED attempt: anchor at (96030, 300); rises 70, dips 230, then the
    // window expires at t=902 — the sample lands in the FAILED population.
    matcher.onTick(96030, 0, at(300));
    CHECK(matcher.stats().attempts == 2);
    matcher.onTick(96100, 0, at(400));
    matcher.onTick(95800, 0, at(460));
    matcher.onTick(96000, 0, at(902));
    CHECK(matcher.activeLeg() == 0);
    REQUIRE(matcher.stats().failedExcursions.size() == 1);
    CHECK(matcher.stats().failedExcursions[0].abovePoints == 70);
    CHECK(matcher.stats().failedExcursions[0].belowPoints == 230);
    // Completed population untouched by the failure.
    CHECK(matcher.stats().completedExcursions.size() == 1);
    CHECK(matcher.stats().completionSeconds.size() == 1);
}

TEST_CASE("spread is sampled at the anchor tick only", "[chainMatcher]") {
    ChainMatcher matcher(chainOf({move(-1.0, 600), move(0.5, 600)}));
    matcher.onTick(100000, 50, at(0));   // pre-anchor spread: ignored
    matcher.onTick(99000, 30, at(60));   // ANCHOR: sampled
    matcher.onTick(99495, 70, at(120));  // post-anchor spread: ignored
    CHECK(matcher.stats().occurrences == 1);
    CHECK(matcher.stats().spreadAtTriggerCount == 1);
    CHECK(matcher.stats().spreadAtTriggerSum == 30);
}

TEST_CASE("the sample cap stops sampling while counts stay exact",
          "[chainMatcher]") {
    // Test-only cap of 2; three single-leg occurrences. Counts stay exact,
    // samples stop at the cap, and the truncation is flagged.
    ChainMatcher matcher(chainOf({move(-1.0, 600)}), /*maxAttemptSamples=*/2);
    std::int32_t price = 100000;
    for (int i = 0; i < 3; ++i) {
        const long t = i * 200;
        matcher.onTick(price, 0, at(t));
        price -= price / 100 + 1;  // just over -1% off the fresh tracker
                                   // (+1 outruns the integer-division floor)
        matcher.onTick(price, 0, at(t + 60));
    }
    CHECK(matcher.stats().occurrences == 3);
    CHECK(matcher.stats().attempts == 3);
    CHECK(matcher.stats().completedExcursions.size() == 2);
    CHECK(matcher.stats().completionSeconds.size() == 2);
    CHECK(matcher.stats().samplesTruncated);
}

TEST_CASE("evaluateExperiment summarises excursions oriented by the final leg",
          "[chainMatcher]") {
    const auto tick = [](const std::int32_t mid, const long seconds) {
        // ask = mid+10 / bid = mid-10: same mid, spread 20 on every tick.
        return PriceData(mid + 10, mid - 10, at(seconds), "EURUSD");
    };
    // The completed-attempt sequence from the recorder test, through the
    // full aggregation: one sample, so p50 == p90 == the sample value.
    const std::vector<PriceData> ticks{
        tick(100000, 0),
        tick(99000, 60),
        tick(98800, 120),
        tick(99495, 180),
    };

    const auto outcome = chain_matcher::evaluateExperiment(
        ticks, chainOf({move(-1.0, 600), move(0.5, 600)}));

    // Final leg rises, so above-anchor is favorable.
    CHECK(outcome.excursionOrientation == "up");
    REQUIRE(outcome.completedAttempts.has_value());
    CHECK(outcome.completedAttempts->samples == 1);
    CHECK(outcome.completedAttempts->mfePoints.p50 == 495.0);
    CHECK(outcome.completedAttempts->mfePoints.p90 == 495.0);
    CHECK(outcome.completedAttempts->maePoints.p50 == 200.0);
    // Percent variants are relative to the attempt's own anchor (99000).
    CHECK(outcome.completedAttempts->mfePercent.p50 == 100.0 * 495 / 99000);
    CHECK(outcome.completedAttempts->maePercent.p50 == 100.0 * 200 / 99000);
    // No failed attempts: null population, not fake zeros.
    CHECK_FALSE(outcome.failedAttempts.has_value());
    REQUIRE(outcome.completionSeconds.has_value());
    CHECK(outcome.completionSeconds->p50 == 120.0);
    REQUIRE(outcome.meanSpreadAtTriggerPoints.has_value());
    CHECK(*outcome.meanSpreadAtTriggerPoints == 20.0);
    CHECK_FALSE(outcome.samplesTruncated);
}

TEST_CASE("a falling final leg flips the excursion orientation", "[chainMatcher]") {
    // Two-leg "drop then keeps dropping": the final leg falls, so
    // BELOW-anchor excursion is the favorable side.
    const auto tick = [](const std::int32_t mid, const long seconds) {
        return PriceData(mid, mid, at(seconds), "EURUSD");
    };
    const std::vector<PriceData> ticks{
        tick(100000, 0),
        tick(99000, 60),   // leg 1: anchor
        tick(98505, 120),  // leg 2: a further -0.5% completes
    };
    const auto outcome = chain_matcher::evaluateExperiment(
        ticks, chainOf({move(-1.0, 600), move(-0.5, 600)}));
    CHECK(outcome.excursionOrientation == "down");
    REQUIRE(outcome.completedAttempts.has_value());
    CHECK(outcome.completedAttempts->mfePoints.p50 == 495.0);  // below anchor
    CHECK(outcome.completedAttempts->maePoints.p50 == 0.0);    // never above
}

TEST_CASE("evaluateExperiment reports per-symbol coverage alongside the v1 map",
          "[chainMatcher]") {
    const auto tick = [](const std::int32_t mid, const long seconds,
                         const char* symbol) {
        return PriceData(mid, mid, at(seconds), symbol);
    };
    // EURUSD: 3 ticks spanning 2 days, one occurrence. GBPUSD: 2 ticks
    // spanning 1 day, none. The per-symbol spans must reflect each symbol's
    // OWN coverage, not the shared stream's.
    const std::vector<PriceData> ticks{
        tick(100000, 0, "EURUSD"),
        tick(100000, 30, "GBPUSD"),
        tick(99000, 60, "EURUSD"),
        tick(99900, 86430, "GBPUSD"),
        tick(99000, 2 * 86400, "EURUSD"),
    };

    const auto outcome = chain_matcher::evaluateExperiment(
        ticks, chainOf({move(-1.0, 600)}));

    REQUIRE(outcome.perSymbol.size() == 2);
    CHECK(outcome.perSymbol.at("EURUSD").occurrences == 1);
    CHECK(outcome.perSymbol.at("EURUSD").ticksScanned == 3);
    CHECK(outcome.perSymbol.at("EURUSD").daysSpanned == 2.0);
    CHECK(outcome.perSymbol.at("GBPUSD").occurrences == 0);
    CHECK(outcome.perSymbol.at("GBPUSD").ticksScanned == 2);
    CHECK(outcome.perSymbol.at("GBPUSD").daysSpanned == 1.0);
    // The flat v1 field stays, and stays consistent with the breakdown.
    CHECK(outcome.occurrencesBySymbol.at("EURUSD") == 1);
    CHECK(outcome.occurrencesBySymbol.at("GBPUSD") == 0);
    // Attempts aggregate across the demuxed matchers (single-leg chain:
    // attempts == occurrences).
    CHECK(outcome.attempts == 1);
}
