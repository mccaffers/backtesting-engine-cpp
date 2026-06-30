// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

import tickPacket;
import priceData;

namespace {

// Build the 40-byte little-endian packet exactly as the C# sender would
// (see the layout in tickPacket.cppm), so the test exercises the real contract.
std::array<std::byte, ingest::kPacketSize> makePacket(double bid, double ask,
                                                      std::int64_t tsMicros,
                                                      const std::string& symbol) {
    std::array<std::byte, ingest::kPacketSize> packet{};

    const auto put = [&](std::size_t offset, auto value) {
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(value)>>(value);
        std::ranges::copy(raw, packet.begin() + offset);
    };
    put(ingest::kBidOffset, bid);
    put(ingest::kAskOffset, ask);
    put(ingest::kTsOffset, tsMicros);

    for (std::size_t i = 0; i < symbol.size() && i < ingest::kSymbolSize; ++i) {
        packet[ingest::kSymbolOffset + i] = static_cast<std::byte>(symbol[i]);
    }
    return packet;
}

// A valid (strictly post-epoch) timestamp for the tests that aren't exercising
// the timestamp guard: 2024-06-26T00:00:00Z = 1719360000 s. decodeTick now drops
// tsMicros <= 0, so these must pass a real date rather than 0.
constexpr std::int64_t kValidTs = 1'719'360'000'000'000LL;

}  // namespace

TEST_CASE("decodeTick parses a packet into scaled PriceData", "[tickPacket]") {
    // 2024-06-26T00:00:00Z = 1719360000 s = 1719360000000000 us.
    const std::int64_t tsMicros = 1'719'360'000'000'000LL;
    const auto packet = makePacket(/*bid=*/1.10000, /*ask=*/1.10001, tsMicros, "EURUSD");

    const auto tick = ingest::decodeTick(packet);
    REQUIRE(tick.has_value());
    CHECK(tick->symbol == "EURUSD");
    // EURUSD price multiplier is 100000: 1.10000 -> 110000, 1.10001 -> 110001.
    CHECK(tick->bid == 110000);
    CHECK(tick->ask == 110001);
    CHECK(std::chrono::duration_cast<std::chrono::microseconds>(
              tick->timestamp.time_since_epoch())
              .count() == tsMicros);
}

TEST_CASE("decodeTick scales by the per-symbol multiplier", "[tickPacket]") {
    SECTION("JPY pair uses x1000") {
        const auto packet = makePacket(156.123, 156.125, kValidTs, "USDJPY");
        const auto tick = ingest::decodeTick(packet);
        REQUIRE(tick.has_value());
        CHECK(tick->bid == 156123);
        CHECK(tick->ask == 156125);
    }
    SECTION("index uses x100") {
        const auto packet = makePacket(5432.10, 5432.20, kValidTs, "USA500IDXUSD");
        const auto tick = ingest::decodeTick(packet);
        REQUIRE(tick.has_value());
        CHECK(tick->bid == 543210);
        CHECK(tick->ask == 543220);
    }
}

TEST_CASE("decodeTick rejects malformed input", "[tickPacket]") {
    SECTION("wrong packet size") {
        std::array<std::byte, 10> tooSmall{};
        CHECK_FALSE(ingest::decodeTick(tooSmall).has_value());
    }
    SECTION("unknown symbol is dropped") {
        const auto packet = makePacket(1.0, 1.0, kValidTs, "NOPE");
        CHECK_FALSE(ingest::decodeTick(packet).has_value());
    }
}

TEST_CASE("decodeTick golden vector pins the on-the-wire byte layout", "[tickPacket]") {
    // The exact 40 bytes of the documented little-endian contract (the byte map
    // in tickPacket.cppm) for bid 1.10000 / ask 1.10001 /
    // ts 2024-06-26T00:00:00Z (1719360000000000 us) / "EURUSD". Unlike the
    // makePacket-based tests, these literals are positional — they do NOT route
    // through kBidOffset/kAskOffset — so a future swap or shift of the decoder's
    // offset constants breaks this test even though the self-consistent tests
    // stay green. (To turn this into a true cross-language check, regenerate
    // these bytes from the real C# Serialize output for the same tick.)
    constexpr std::array<std::byte, ingest::kPacketSize> golden{
        std::byte{0x9A}, std::byte{0x99}, std::byte{0x99}, std::byte{0x99},
        std::byte{0x99}, std::byte{0x99}, std::byte{0xF1}, std::byte{0x3F},  // bid 1.10000
        std::byte{0x0B}, std::byte{0x5E}, std::byte{0xF4}, std::byte{0x15},
        std::byte{0xA4}, std::byte{0x99}, std::byte{0xF1}, std::byte{0x3F},  // ask 1.10001
        std::byte{0x00}, std::byte{0x80}, std::byte{0x0A}, std::byte{0xB2},
        std::byte{0xBF}, std::byte{0x1B}, std::byte{0x06}, std::byte{0x00},  // ts 1719360000000000
        std::byte{0x45}, std::byte{0x55}, std::byte{0x52}, std::byte{0x55},
        std::byte{0x53}, std::byte{0x44}, std::byte{0x00}, std::byte{0x00},  // "EURUSD"
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };

    const auto tick = ingest::decodeTick(golden);
    REQUIRE(tick.has_value());
    CHECK(tick->symbol == "EURUSD");
    CHECK(tick->bid == 110000);
    CHECK(tick->ask == 110001);
    CHECK(std::chrono::duration_cast<std::chrono::microseconds>(
              tick->timestamp.time_since_epoch())
              .count() == 1'719'360'000'000'000LL);
}

TEST_CASE("decodeTick drops corrupt ticks (hardening guards)", "[tickPacket]") {
    SECTION("zero bid is dropped") {
        const auto packet = makePacket(/*bid=*/0.0, /*ask=*/1.10001, kValidTs, "EURUSD");
        CHECK_FALSE(ingest::decodeTick(packet).has_value());
    }
    SECTION("zero ask is dropped") {
        const auto packet = makePacket(/*bid=*/1.10000, /*ask=*/0.0, kValidTs, "EURUSD");
        CHECK_FALSE(ingest::decodeTick(packet).has_value());
    }
    SECTION("zero (epoch) timestamp is dropped") {
        const auto packet = makePacket(1.10000, 1.10001, /*tsMicros=*/0, "EURUSD");
        CHECK_FALSE(ingest::decodeTick(packet).has_value());
    }
    SECTION("negative timestamp is dropped") {
        const auto packet = makePacket(1.10000, 1.10001, /*tsMicros=*/-1, "EURUSD");
        CHECK_FALSE(ingest::decodeTick(packet).has_value());
    }
    SECTION("price that overflows INT32 when scaled is dropped") {
        // EURUSD scales x100000; INT32 max is 2,147,483,647, so any price above
        // ~21474.83 overflows the scaled integer. A real EURUSD quote never gets
        // near this (hence "latent"), but the guard must still reject it rather
        // than store a truncated value. 30000 * 100000 = 3,000,000,000 > INT32.
        const auto packet = makePacket(/*bid=*/1.10000, /*ask=*/30000.0, kValidTs, "EURUSD");
        CHECK_FALSE(ingest::decodeTick(packet).has_value());
    }
}
