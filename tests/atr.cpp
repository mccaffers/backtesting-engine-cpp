#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

import atr;
import ohlcObject;

namespace {

// ATR only reads high/low/close; open mirrors close and date is irrelevant.
OhlcObject candle(std::int32_t high, std::int32_t low, std::int32_t close) {
    return OhlcObject{.open = close, .close = close, .high = high, .low = low};
}

}  // namespace

TEST_CASE("atr::calculate matches the C# reference shape", "[atr]") {
    SECTION("fewer than period+1 candles returns 0 (not warm)") {
        std::vector<OhlcObject> candles;
        CHECK(atr::calculate(candles, 3) == 0);
        for (int i = 0; i < 3; ++i) {
            candles.push_back(candle(110010, 109990, 110000));
        }
        CHECK(candles.size() == 3);  // period 3 needs 4
        CHECK(atr::calculate(candles, 3) == 0);
    }

    SECTION("contained ranges: high-low dominates, SMA of the TRs") {
        const std::vector<OhlcObject> candles{
            candle(100, 100, 100),
            candle(110, 95, 100),   // prevClose 100 inside -> TR = 15
            candle(105, 98, 100),   // TR = 7
        };
        CHECK(atr::calculate(candles, 2) == 11);  // (15 + 7) / 2
    }

    SECTION("gap up: |high - prevClose| dominates") {
        const std::vector<OhlcObject> candles{
            candle(100, 100, 100),
            candle(130, 125, 128),  // TR = max(5, 30, 25) = 30
        };
        CHECK(atr::calculate(candles, 1) == 30);
    }

    SECTION("gap down: |low - prevClose| dominates") {
        const std::vector<OhlcObject> candles{
            candle(100, 100, 100),
            candle(80, 70, 75),     // TR = max(10, 20, 30) = 30
        };
        CHECK(atr::calculate(candles, 1) == 30);
    }

    SECTION("non-divisible sums round to the nearest point") {
        const std::vector<OhlcObject> up{
            candle(100, 100, 100),
            candle(110, 100, 100),  // TR = 10
            candle(110, 95, 100),   // TR = 15
        };
        CHECK(atr::calculate(up, 2) == 13);  // 12.5 rounds up

        const std::vector<OhlcObject> down{
            candle(100, 100, 100),
            candle(110, 100, 100),  // TR = 10
            candle(110, 100, 100),  // TR = 10
            candle(111, 100, 100),  // TR = 11
        };
        CHECK(atr::calculate(down, 3) == 10);  // 31/3 = 10.33 rounds down
    }

    SECTION("only the last period+1 candles are read") {
        const std::vector<OhlcObject> candles{
            candle(2000, 1, 500),    // wild history that must not leak in
            candle(3000, 1, 1000),
            candle(100, 100, 100),   // window starts here (prevClose source)
            candle(110, 100, 105),   // TR = 10
            candle(115, 105, 110),   // TR = 10
        };
        CHECK(atr::calculate(candles, 2) == 10);
    }

    SECTION("period below 1 throws") {
        const std::vector<OhlcObject> candles{candle(100, 100, 100),
                                              candle(110, 100, 105)};
        CHECK_THROWS_AS(atr::calculate(candles, 0), std::invalid_argument);
        CHECK_THROWS_AS(atr::calculate(candles, -1), std::invalid_argument);
    }

    SECTION("index-scale prices don't overflow") {
        // ~4.5M points is the biggest value the engine stores (indices).
        std::vector<OhlcObject> candles{candle(4'500'000, 4'500'000, 4'500'000)};
        for (int i = 0; i < 14; ++i) {
            candles.push_back(candle(4'550'000, 4'450'000, 4'500'000));
        }
        CHECK(atr::calculate(candles, 14) == 100'000);
    }
}
