// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>

import tickLine;
import tickPacket;
import priceData;

namespace {

// 2024-06-26T00:00:00Z = 1719360000000000 us = 1719360000000000000 ns.
tick_packet::DecodedTick makeTick(const std::string& source) {
    const auto ts = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::microseconds(1'719'360'000'000'000LL)));
    return {PriceData(/*ask=*/110001, /*bid=*/110000, ts, "EURUSD"), source};
}

}  // namespace

TEST_CASE("formatLine tags known sources", "[tickLine]") {
    CHECK(tick_line::formatLine(makeTick("IG")) ==
          "EURUSD,source=IG ask=110001i,bid=110000i 1719360000000000000\n");
    CHECK(tick_line::formatLine(makeTick("MX")) ==
          "EURUSD,source=MX ask=110001i,bid=110000i 1719360000000000000\n");
}

TEST_CASE("formatLine omits the tag for an empty or unknown source", "[tickLine]") {
    const std::string untagged = "EURUSD ask=110001i,bid=110000i 1719360000000000000\n";
    CHECK(tick_line::formatLine(makeTick("")) == untagged);
    CHECK(tick_line::formatLine(makeTick("XX")) == untagged);
    // Would need ILP escaping if it were ever written as a tag value.
    CHECK(tick_line::formatLine(makeTick("I G,x=1")) == untagged);
}

TEST_CASE("isKnownSource accepts only the producer feed ids", "[tickLine]") {
    CHECK(tick_line::isKnownSource("IG"));
    CHECK(tick_line::isKnownSource("MX"));
    CHECK_FALSE(tick_line::isKnownSource(""));
    CHECK_FALSE(tick_line::isKnownSource("ig"));
    CHECK_FALSE(tick_line::isKnownSource("IGX"));
}
