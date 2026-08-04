// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <string>

import dealPacket;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The producer-side view of one deal (vortex/shared/DealSerializer.cs):
// doubles default to NaN (= absent on the wire), strings to empty.
struct Wire {
    double level = kNaN;
    double size = kNaN;
    double stopLevel = kNaN;
    double limitLevel = kNaN;
    std::string dealReference;
    std::string dealId;
    std::string dealIdOrigin;
    std::string epic;
    std::string direction;
    std::string status;
    std::string dealStatus;
    std::string currency;
    std::string channel;
    std::string expiry;
    std::string timestamp;
    std::string guaranteedStop;
};

// Build the 256-byte packet exactly as DealSerializer.Serialize does:
// little-endian doubles, ASCII strings NUL-padded and capped at field
// size - 1. The offsets are the documented wire positions written as literals
// — deliberately NOT the decoder's own layout — so every test through this
// helper also pins the decoder's field positions against the contract.
std::array<std::byte, deal_packet::kPacketSize> makePacket(const Wire& wire) {
    std::array<std::byte, deal_packet::kPacketSize> packet{};

    const auto putDouble = [&](std::size_t offset, double value) {
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(double)>>(value);
        std::ranges::copy(raw, packet.begin() + offset);
    };
    const auto putString = [&](std::size_t offset, std::size_t size, const std::string& s) {
        const std::size_t n = std::min(s.size(), size - 1);  // always keep a NUL
        for (std::size_t i = 0; i < n; ++i) {
            packet[offset + i] = static_cast<std::byte>(s[i]);
        }
    };

    putDouble(0, wire.level);
    putDouble(8, wire.size);
    putDouble(16, wire.stopLevel);
    putDouble(24, wire.limitLevel);
    putString(32, 32, wire.dealReference);
    putString(64, 32, wire.dealId);
    putString(96, 32, wire.dealIdOrigin);
    putString(128, 32, wire.epic);
    putString(160, 8, wire.direction);
    putString(168, 12, wire.status);
    putString(180, 12, wire.dealStatus);
    putString(192, 4, wire.currency);
    putString(196, 16, wire.channel);
    putString(212, 8, wire.expiry);
    putString(220, 24, wire.timestamp);
    putString(244, 8, wire.guaranteedStop);
    return packet;
}

}  // namespace

TEST_CASE("decodeDeal parses a fully populated packet", "[dealPacket]") {
    const auto packet = makePacket({
        .level = 8123.5,
        .size = 1.0,
        .stopLevel = 8100.0,
        .limitLevel = 8150.0,
        .dealReference = "9FKSX2Y5S8NT2E4",
        .dealId = "DIAAAAUD3HG2JA6",
        .dealIdOrigin = "DIAAAAUD3HG2JA5",
        .epic = "IX.D.ASX.IFS.IP",
        .direction = "BUY",
        .status = "OPEN",
        .dealStatus = "ACCEPTED",
        .currency = "GBP",
        .channel = "PublicRestOTC",
        .expiry = "DFB",
        .timestamp = "2026-07-11T09:15:03.123",
        .guaranteedStop = "false",
    });

    const auto deal = deal_packet::decodeDeal(packet);
    REQUIRE(deal.has_value());
    REQUIRE(deal->level.has_value());
    CHECK(*deal->level == 8123.5);
    REQUIRE(deal->size.has_value());
    CHECK(*deal->size == 1.0);
    REQUIRE(deal->stopLevel.has_value());
    CHECK(*deal->stopLevel == 8100.0);
    REQUIRE(deal->limitLevel.has_value());
    CHECK(*deal->limitLevel == 8150.0);
    CHECK(deal->dealReference == "9FKSX2Y5S8NT2E4");
    CHECK(deal->dealId == "DIAAAAUD3HG2JA6");
    CHECK(deal->dealIdOrigin == "DIAAAAUD3HG2JA5");
    CHECK(deal->epic == "IX.D.ASX.IFS.IP");
    CHECK(deal->direction == "BUY");
    CHECK(deal->status == "OPEN");
    CHECK(deal->dealStatus == "ACCEPTED");
    CHECK(deal->currency == "GBP");
    CHECK(deal->channel == "PublicRestOTC");
    CHECK(deal->expiry == "DFB");
    CHECK(deal->timestamp == "2026-07-11T09:15:03.123");
    CHECK(deal->guaranteedStop == "false");
}

TEST_CASE("decodeDeal maps absent values to their empty forms", "[dealPacket]") {
    // All doubles NaN, all strings empty — the producer's shape for a deal
    // where IG omitted every optional field.
    const auto deal = deal_packet::decodeDeal(makePacket({.status = "DELETED"}));
    REQUIRE(deal.has_value());
    CHECK_FALSE(deal->level.has_value());
    CHECK_FALSE(deal->size.has_value());
    CHECK_FALSE(deal->stopLevel.has_value());
    CHECK_FALSE(deal->limitLevel.has_value());
    CHECK(deal->dealReference.empty());
    CHECK(deal->epic.empty());
    CHECK(deal->status == "DELETED");
    CHECK(deal->guaranteedStop.empty());
}

TEST_CASE("decodeDeal passes field content through unjudged", "[dealPacket]") {
    // Unlike ticks there is no plausibility gate: a zero or negative level is
    // a value IG sent, not corruption — present it, don't drop the deal.
    const auto deal = deal_packet::decodeDeal(makePacket({.level = 0.0, .size = -2.5}));
    REQUIRE(deal.has_value());
    REQUIRE(deal->level.has_value());
    CHECK(*deal->level == 0.0);
    REQUIRE(deal->size.has_value());
    CHECK(*deal->size == -2.5);
}

TEST_CASE("decodeDeal rejects wrong-sized datagrams", "[dealPacket]") {
    SECTION("too small") {
        std::array<std::byte, deal_packet::kPacketSize - 1> tooSmall{};
        CHECK_FALSE(deal_packet::decodeDeal(tooSmall).has_value());
    }
    SECTION("too large") {
        std::array<std::byte, deal_packet::kPacketSize + 1> tooLarge{};
        CHECK_FALSE(deal_packet::decodeDeal(tooLarge).has_value());
    }
    SECTION("a 40-byte tick packet is not a deal") {
        std::array<std::byte, 40> tick{};
        CHECK_FALSE(deal_packet::decodeDeal(tick).has_value());
    }
}

TEST_CASE("decodeDeal hardening against a non-conforming producer", "[dealPacket]") {
    SECTION("an unterminated field yields the full width, no overread") {
        // The producer guarantees a NUL by capping content at size - 1; if a
        // future producer breaks that, the decoder must still stop at the
        // field edge rather than run into the neighbouring field.
        auto packet = makePacket({.direction = "BUY"});
        for (std::size_t i = 64; i < 96; ++i) {  // fill dealId wall to wall
            packet[i] = static_cast<std::byte>('A');
        }
        const auto deal = deal_packet::decodeDeal(packet);
        REQUIRE(deal.has_value());
        CHECK(deal->dealId == std::string(32, 'A'));
        CHECK(deal->dealIdOrigin.empty());  // neighbour untouched
        CHECK(deal->direction == "BUY");
    }
    SECTION("non-zero reserved bytes do not reject the deal") {
        auto packet = makePacket({.status = "OPEN"});
        packet[252] = static_cast<std::byte>(0xFF);
        packet[255] = static_cast<std::byte>(0x01);
        const auto deal = deal_packet::decodeDeal(packet);
        REQUIRE(deal.has_value());
        CHECK(deal->status == "OPEN");
    }
}

TEST_CASE("makePacket mirrors the producer's size-1 truncation", "[dealPacket]") {
    // DealSerializer caps content at field size - 1 so a NUL always survives;
    // a 40-char epic therefore arrives as its first 31 chars.
    const std::string longEpic(40, 'E');
    const auto deal = deal_packet::decodeDeal(makePacket({.epic = longEpic}));
    REQUIRE(deal.has_value());
    CHECK(deal->epic == std::string(31, 'E'));
}
