// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// dealPacket — the on-the-wire UDP deal/account-update format and its decoder.
//
// When the IG Lightstreamer feed pushes a TRADE:* account update (an OPU
// field), the C# producer (vortex igmarkets/SubListener.cs) parses it into a
// Deal and shared/DealSerializer.cs fires it at udp_ports::kTrade as a single
// datagram — the same hand-rolled, fixed-size, little-endian contract as the
// tick feed (tickPacket), on its own port. One datagram = one deal, exactly
// kPacketSize bytes; anything else is discarded.
//
//   offset size type     field
//     0     8   double   level           NaN when absent
//     8     8   double   size            NaN when absent
//    16     8   double   stopLevel       NaN when absent
//    24     8   double   limitLevel      NaN when absent
//    32    32   char[]   dealReference
//    64    32   char[]   dealId
//    96    32   char[]   dealIdOrigin
//   128    32   char[]   epic            e.g. "IX.D.ASX.IFS.IP"
//   160     8   char[]   direction       "BUY" | "SELL"
//   168    12   char[]   status          "OPEN" | "UPDATED" | "DELETED"
//   180    12   char[]   dealStatus      "ACCEPTED" | "REJECTED"
//   192     4   char[]   currency        e.g. "GBP"
//   196    16   char[]   channel         e.g. "WTP", "PublicRestOTC"
//   212     8   char[]   expiry          e.g. "-", "DFB"
//   220    24   char[]   timestamp       IG's raw "yyyy-MM-ddTHH:mm:ss.fff"
//   244     8   char[]   guaranteedStop  "true" | "false" | ""
//   252     4   —        reserved        always zero
//
// String fields are ASCII, NUL-padded, and always NUL-terminated (the producer
// truncates content to field size - 1); an absent value is an empty field.
// decodeDeal surfaces the NaN-when-absent doubles as std::optional and the
// char fields as std::string, so consumers never touch the raw layout.

module;

#include <cstddef>  // offsetof is a macro, so `import std` alone can't supply it

export module dealPacket;

import std;  // <algorithm>, <array>, <bit>, <cmath>, <optional>, <span>, <string>

export namespace deal_packet {

inline constexpr std::size_t kPacketSize = 256;  // == DealSerializer.PacketSize

namespace detail {

#pragma pack(push, 1)
// Byte-for-byte overlay of the wire packet (the byte map above). The deploy
// targets (x86-64, aarch64) are little-endian and every double sits at a
// naturally aligned offset, so a bit_cast of the datagram fills it directly —
// no per-field readLE walk like the 3-field tick packet needs.
struct DealMessage {
    double level;
    double size;
    double stopLevel;
    double limitLevel;
    char dealReference[32];
    char dealId[32];
    char dealIdOrigin[32];
    char epic[32];
    char direction[8];
    char status[12];
    char dealStatus[12];
    char currency[4];
    char channel[16];
    char expiry[8];
    char timestamp[24];
    char guaranteedStop[8];
    char reserved[4];
};
#pragma pack(pop)

// Pin the overlay to the documented contract at compile time — a reordered or
// resized member can't silently shift every field after it.
static_assert(sizeof(DealMessage) == kPacketSize, "wire format is 256 bytes");
static_assert(offsetof(DealMessage, dealReference) == 32);
static_assert(offsetof(DealMessage, dealId) == 64);
static_assert(offsetof(DealMessage, dealIdOrigin) == 96);
static_assert(offsetof(DealMessage, epic) == 128);
static_assert(offsetof(DealMessage, direction) == 160);
static_assert(offsetof(DealMessage, status) == 168);
static_assert(offsetof(DealMessage, dealStatus) == 180);
static_assert(offsetof(DealMessage, currency) == 192);
static_assert(offsetof(DealMessage, channel) == 196);
static_assert(offsetof(DealMessage, expiry) == 212);
static_assert(offsetof(DealMessage, timestamp) == 220);
static_assert(offsetof(DealMessage, guaranteedStop) == 244);
static_assert(offsetof(DealMessage, reserved) == 252);

// NaN is the producer's "absent" marker for the optional doubles.
[[nodiscard]] inline std::optional<double> presentOrNullopt(const double value) noexcept {
    if (std::isnan(value)) {
        return std::nullopt;
    }
    return value;
}

// Fixed-width, NUL-padded ASCII field -> owned string, trimmed at the first
// NUL. The producer always NUL-terminates, but a full-width field (no NUL) is
// still safe — the copy never reads past the array.
template <std::size_t N>
[[nodiscard]] std::string toString(const char (&field)[N]) {
    const auto end = std::find(std::begin(field), std::end(field), '\0');
    return {std::begin(field), end};
}

}  // namespace detail

// One decoded deal/account update, with the wire's absence conventions
// (NaN doubles, empty strings) mapped to friendly types.
struct Deal {
    std::optional<double> level;       // nullopt when IG sent no value
    std::optional<double> size;        // nullopt when IG sent no value
    std::optional<double> stopLevel;   // nullopt when IG sent no value
    std::optional<double> limitLevel;  // nullopt when IG sent no value
    std::string dealReference;
    std::string dealId;
    std::string dealIdOrigin;
    std::string epic;            // e.g. "IX.D.ASX.IFS.IP"
    std::string direction;       // "BUY" | "SELL"
    std::string status;          // "OPEN" | "UPDATED" | "DELETED"
    std::string dealStatus;      // "ACCEPTED" | "REJECTED"
    std::string currency;        // e.g. "GBP"
    std::string channel;         // e.g. "WTP", "PublicRestOTC"
    std::string expiry;          // e.g. "-", "DFB"
    std::string timestamp;       // IG's raw "yyyy-MM-ddTHH:mm:ss.fff" string
    std::string guaranteedStop;  // "true" | "false" | "" (absent)
};

// Decode one datagram. Returns nullopt — i.e. "drop it", since UDP is
// best-effort and a stray datagram must not take the receiver down — only for
// a wrong-sized packet, the single structural rule of the contract. Field
// content is passed through as-is: unlike ticks there is no price/timestamp
// plausibility gate here, because a deal is an account event whose meaning
// (including absent fields) is for the tracking consumer to judge.
[[nodiscard]] inline std::optional<Deal> decodeDeal(std::span<const std::byte> bytes) {
    if (bytes.size() != kPacketSize) {
        return std::nullopt;
    }

    // bit_cast keeps the overlay well-defined (no reinterpret_cast aliasing UB);
    // on the little-endian deploy targets the bytes map straight onto the
    // native scalars with no swapping.
    std::array<std::byte, kPacketSize> raw{};
    std::ranges::copy(bytes, raw.begin());
    const auto msg = std::bit_cast<detail::DealMessage>(raw);

    Deal deal;
    deal.level = detail::presentOrNullopt(msg.level);
    deal.size = detail::presentOrNullopt(msg.size);
    deal.stopLevel = detail::presentOrNullopt(msg.stopLevel);
    deal.limitLevel = detail::presentOrNullopt(msg.limitLevel);
    deal.dealReference = detail::toString(msg.dealReference);
    deal.dealId = detail::toString(msg.dealId);
    deal.dealIdOrigin = detail::toString(msg.dealIdOrigin);
    deal.epic = detail::toString(msg.epic);
    deal.direction = detail::toString(msg.direction);
    deal.status = detail::toString(msg.status);
    deal.dealStatus = detail::toString(msg.dealStatus);
    deal.currency = detail::toString(msg.currency);
    deal.channel = detail::toString(msg.channel);
    deal.expiry = detail::toString(msg.expiry);
    deal.timestamp = detail::toString(msg.timestamp);
    deal.guaranteedStop = detail::toString(msg.guaranteedStop);
    return deal;
}

}  // namespace deal_packet
