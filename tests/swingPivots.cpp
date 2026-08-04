#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

import swingPivots;
import ohlcObject;

namespace {

// The predicates only read highs/lows; open/close mirror the midpoint and
// date is irrelevant.
OhlcObject candle(std::int32_t high, std::int32_t low) {
    const std::int32_t mid = (high + low) / 2;
    return OhlcObject{.open = mid, .close = mid, .high = high, .low = low};
}

}  // namespace

TEST_CASE("swing_pivots detects strict fractal extremes", "[swingPivots]") {
    // Index 2 is a 2-wing swing high (110070 beats both wings) and index 2's
    // low is NOT a swing low (109990 is beaten by index 4's 109950).
    const std::vector<OhlcObject> bars{
        candle(110020, 109980),
        candle(110040, 110000),
        candle(110070, 109990),
        candle(110030, 109970),
        candle(110010, 109950),
    };

    SECTION("swing high: strictly above both wings") {
        CHECK(swing_pivots::isSwingHighAt(bars, 2, 2));
        CHECK(swing_pivots::isSwingHighAt(bars, 2, 1));
    }

    SECTION("non-extremes are not pivots") {
        CHECK_FALSE(swing_pivots::isSwingHighAt(bars, 1, 1));
        CHECK_FALSE(swing_pivots::isSwingLowAt(bars, 2, 2));
    }

    SECTION("swing low: strictly below both wings") {
        const std::vector<OhlcObject> lows{
            candle(110020, 109980),
            candle(110040, 110000),
            candle(110010, 109940),
            candle(110030, 109970),
            candle(110010, 109990),
        };
        CHECK(swing_pivots::isSwingLowAt(lows, 2, 2));
        CHECK_FALSE(swing_pivots::isSwingHighAt(lows, 2, 2));
    }
}

TEST_CASE("swing_pivots ties disqualify", "[swingPivots]") {
    // An equalled extreme is no fresh extreme (double top): index 2 matches
    // index 0's high exactly, so neither is a pivot at wing 2.
    const std::vector<OhlcObject> bars{
        candle(110070, 109980),
        candle(110040, 110000),
        candle(110070, 109990),
        candle(110030, 109970),
        candle(110010, 109960),
    };
    CHECK_FALSE(swing_pivots::isSwingHighAt(bars, 2, 2));

    // At wing 1 the tie sits outside the window, so index 2 qualifies again —
    // the wing width bounds what an extreme is compared against.
    CHECK(swing_pivots::isSwingHighAt(bars, 2, 1));
}

TEST_CASE("swing_pivots wing width changes the verdict", "[swingPivots]") {
    // Index 2 beats its immediate neighbours but not the edge bars: a pivot
    // at wing 1, not at wing 2.
    const std::vector<OhlcObject> bars{
        candle(110100, 109980),
        candle(110040, 110000),
        candle(110070, 109990),
        candle(110030, 109970),
        candle(110090, 109960),
    };
    CHECK(swing_pivots::isSwingHighAt(bars, 2, 1));
    CHECK_FALSE(swing_pivots::isSwingHighAt(bars, 2, 2));
}

TEST_CASE("swing_pivots rejects a wingless pivot", "[swingPivots]") {
    const std::vector<OhlcObject> bars{
        candle(110020, 109980),
        candle(110040, 110000),
        candle(110030, 109990),
    };
    CHECK_THROWS_AS(swing_pivots::isSwingHighAt(bars, 1, 0), std::invalid_argument);
    CHECK_THROWS_AS(swing_pivots::isSwingLowAt(bars, 1, -1), std::invalid_argument);
}
