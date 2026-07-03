#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

import ema;

TEST_CASE("ema::calculate matches the C# reference shape", "[ema]") {
    SECTION("pads with zeros, seeds with the SMA, then follows the recurrence") {
        // period 3: multiplier 2/(3+1) = 0.5, so each step is the simple
        // midpoint of the new price and the previous EMA — exact integers.
        const std::vector<std::int32_t> prices{10, 20, 30, 40, 50};
        const auto out = ema::calculate(prices, 3);

        REQUIRE(out.size() == prices.size());
        CHECK(out[0] == 0);
        CHECK(out[1] == 0);
        CHECK(out[2] == 20);  // SMA(10, 20, 30)
        CHECK(out[3] == 30);  // 0.5*40 + 0.5*20
        CHECK(out[4] == 40);  // 0.5*50 + 0.5*30
    }

    SECTION("fractional values round to the nearest point") {
        // period 2: seed SMA(10, 11) = 10.5 -> 11; then
        // ema = (2*12 + 1*10.5) / 3 = 11.5 -> 12.
        const std::vector<std::int32_t> prices{10, 11, 12};
        const auto out = ema::calculate(prices, 2);

        REQUIRE(out.size() == 3);
        CHECK(out[0] == 0);
        CHECK(out[1] == 11);
        CHECK(out[2] == 12);
    }

    SECTION("a constant series settles on the constant") {
        const std::vector<std::int32_t> prices(6, 110001);
        const auto out = ema::calculate(prices, 3);

        REQUIRE(out.size() == 6);
        CHECK(out[0] == 0);
        CHECK(out[1] == 0);
        for (std::size_t i = 2; i < out.size(); ++i) {
            CHECK(out[i] == 110001);
        }
    }

    SECTION("largest scaled prices don't overflow the Q16 state") {
        // ~4.5M points is the biggest value the engine stores (indices).
        const std::vector<std::int32_t> prices(4, 4'500'000);
        const auto out = ema::calculate(prices, 3);

        REQUIRE(out.size() == 4);
        CHECK(out[2] == 4'500'000);
        CHECK(out[3] == 4'500'000);
    }
}

TEST_CASE("ema::calculate stays within one point of exact arithmetic", "[ema]") {
    // Reference EMA in double (fine in a test; the production path is what
    // must stay integer). The integer Q16 recurrence should track it to <= 1
    // point at every index, even over a long, drifting series.
    std::vector<std::int32_t> prices;
    for (int i = 0; i < 60; ++i) {
        prices.push_back(110000 + i * 7 + (i % 5) * 13);
    }
    const int period = 9;
    const auto out = ema::calculate(prices, period);
    REQUIRE(out.size() == prices.size());

    double emaD = 0.0;
    for (int i = 0; i < period; ++i) {
        emaD += prices[static_cast<std::size_t>(i)];
    }
    emaD /= period;
    CHECK(std::llabs(out[period - 1] - std::llround(emaD)) <= 1);

    for (std::size_t i = static_cast<std::size_t>(period); i < prices.size(); ++i) {
        emaD = (2.0 * prices[i] + (period - 1) * emaD) / (period + 1);
        INFO("index " << i);
        CHECK(std::llabs(out[i] - std::llround(emaD)) <= 1);
    }
}

TEST_CASE("ema::calculate never freezes on small moves (no integer sticking)", "[ema]") {
    SECTION("a 1-point-per-step ramp keeps the EMA moving") {
        // With whole-point state, alpha * 1 point would truncate to zero and
        // the indicator would freeze at the seed. The Q16 state must keep
        // climbing the whole way.
        std::vector<std::int32_t> prices;
        for (int i = 0; i < 40; ++i) {
            prices.push_back(110000 + i);
        }
        const int period = 9;
        const auto out = ema::calculate(prices, period);

        for (std::size_t i = static_cast<std::size_t>(period); i < out.size(); ++i) {
            INFO("index " << i);
            CHECK(out[i] >= out[i - 1]);
        }
        CHECK(out.back() > out[static_cast<std::size_t>(period) - 1]);
    }

    SECTION("after the input flattens, the EMA converges onto the constant") {
        std::vector<std::int32_t> prices{110100, 110080, 110060, 110040, 110020};
        for (int i = 0; i < 30; ++i) {
            prices.push_back(110000);
        }
        const auto out = ema::calculate(prices, 5);

        CHECK(out.back() == 110000);
        // ...and holds there rather than oscillating or drifting.
        CHECK(out[out.size() - 2] == 110000);
        CHECK(out[out.size() - 3] == 110000);
    }
}

TEST_CASE("ema::calculate edge cases", "[ema]") {
    SECTION("fewer prices than the period yields all zeros") {
        const std::vector<std::int32_t> prices{100, 200};
        const auto out = ema::calculate(prices, 5);
        CHECK(out == std::vector<std::int32_t>{0, 0});
    }

    SECTION("empty input yields empty output") {
        CHECK(ema::calculate(std::vector<std::int32_t>{}, 3).empty());
    }

    SECTION("period below 1 throws") {
        const std::vector<std::int32_t> prices{1, 2, 3};
        CHECK_THROWS_AS(ema::calculate(prices, 0), std::invalid_argument);
        CHECK_THROWS_AS(ema::calculate(prices, -2), std::invalid_argument);
    }

    SECTION("the out-param overload reuses the buffer and agrees with the returning one") {
        const std::vector<std::int32_t> first{10, 20, 30, 40, 50};
        const std::vector<std::int32_t> second{5, 6, 7};

        std::vector<std::int32_t> out;
        ema::calculate(first, 3, out);
        CHECK(out == ema::calculate(first, 3));

        ema::calculate(second, 2, out);  // same buffer, shorter input
        CHECK(out == ema::calculate(second, 2));
    }
}
