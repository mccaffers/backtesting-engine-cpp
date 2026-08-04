// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// tickPacket — the on-the-wire UDP tick format and its decoder.
//
// Replaces the C# side's MemoryPack serialisation (which has no C++ port) with a
// fixed, language-neutral binary layout that both ends produce/parse by hand.
// Each datagram is exactly kPacketSize bytes, little-endian:
//
//   offset size field
//     0     8   double  bid          // raw price, e.g. 1.10000
//     8     8   double  ask
//    16     8   int64   tsMicros     // Unix epoch microseconds, UTC
//    24    16   char    symbol[16]   // ASCII, NUL-padded, e.g. "EURUSD\0\0..."
//                                    // total = 40 bytes
//
// x86-64 and ARM64 (the only deploy targets) are little-endian, so the bytes map
// straight onto the native scalars with no byte-swapping; the C# sender writes
// little-endian explicitly to make that contract intentional.
//
// decodeTick turns those bytes into the engine's existing PriceData: it scales
// the real bid/ask into the stored INT32 "points" via the symbol's price
// multiplier (symbol_scale::getPriceScale) so the ingest writes exactly what the
// backtester's read path already expects. Shared by the ingest and live
// subcommands — both consume the same wire format, on different ports.

export module tickPacket;

import std;        // <array>, <bit>, <chrono>, <cstddef>, <cstdint>, <limits>,
                   // <optional>, <span>, <string>, <algorithm>, <cmath>
import priceData;  // PriceData
import symbolScale; // symbol_scale::getPriceScale, kUnknown

export namespace tick_packet {

// Fixed packet geometry (see the header comment for the byte map).
inline constexpr std::size_t kBidOffset    = 0;
inline constexpr std::size_t kAskOffset    = 8;
inline constexpr std::size_t kTsOffset     = 16;
inline constexpr std::size_t kSymbolOffset = 24;
inline constexpr std::size_t kSymbolSize   = 16;
inline constexpr std::size_t kPacketSize   = 40;

namespace detail {

// Reinterpret the first sizeof(T) bytes of `bytes` as a little-endian T. On the
// little-endian deploy targets this is a straight bit_cast (no swap); bit_cast
// keeps it well-defined (no reinterpret_cast / aliasing UB).
template <class T>
[[nodiscard]] T readLE(std::span<const std::byte> bytes) noexcept {
    std::array<std::byte, sizeof(T)> raw{};
    std::ranges::copy(bytes.first(sizeof(T)), raw.begin());
    return std::bit_cast<T>(raw);
}

}  // namespace detail

// Decode one datagram into a PriceData. Returns nullopt — i.e. "drop this tick"
// rather than a hard error, since UDP is best-effort and a stray/garbled or
// malformed datagram must not take the ingest down — for any of:
//   - a wrong-sized packet,
//   - a bid or ask that isn't finite and strictly positive — zero is the
//     bad-data signature found in the historical feed (the C# producer already
//     guards it at SubListener.cs:126; we re-check here as defense in depth
//     against a future non-guarding producer), and NaN/Inf/negatives are the
//     same class of corrupt/unset data,
//   - an out-of-range timestamp (<= 0 is unset / pre-1970; an implausibly large
//     value is corrupt and would overflow the micros->nanos conversion on the
//     write path),
//   - an unknown symbol (multiplier 0),
//   - a scaled price that overflows INT32 (latent: real prices sit ~300x below
//     the limit, but we never silently store a truncated value).
[[nodiscard]] inline std::optional<PriceData> decodeTick(std::span<const std::byte> bytes) {
    if (bytes.size() != kPacketSize) {
        return std::nullopt;
    }

    const double bid = detail::readLE<double>(bytes.subspan(kBidOffset));
    const double ask = detail::readLE<double>(bytes.subspan(kAskOffset));
    const std::int64_t tsMicros = detail::readLE<std::int64_t>(bytes.subspan(kTsOffset));

    // Drop anything that isn't a real, tradable price. A genuine quote is always
    // finite and strictly positive, so this one test rejects every corrupt/unset
    // signature at once: NaN/Inf (which would otherwise sail past an == 0 check
    // and give std::llround unspecified behaviour), zero, and negatives.
    if (!std::isfinite(bid) || !std::isfinite(ask) || bid <= 0.0 || ask <= 0.0) {
        return std::nullopt;
    }

    // Drop out-of-range timestamps. A real tick is strictly after the Unix epoch
    // (<= 0 is the null-date signature), and the write path converts these micros
    // to nanoseconds (x1000) — so a corrupt, implausibly-large value would
    // overflow int64 there (signed-overflow UB, garbage timestamp). Bound both
    // ends; kMaxTsMicros is 2100-01-01Z, far past any real feed.
    constexpr std::int64_t kMaxTsMicros = 4'102'444'800'000'000;
    if (tsMicros <= 0 || tsMicros > kMaxTsMicros) {
        return std::nullopt;
    }

    // Symbol: NUL-padded ASCII in a fixed 16-byte field. Copy out the bytes and
    // trim at the first NUL.
    std::array<char, kSymbolSize> symBuf{};
    std::ranges::transform(bytes.subspan(kSymbolOffset, kSymbolSize), symBuf.begin(),
                           [](std::byte b) { return static_cast<char>(b); });
    const auto nul = std::ranges::find(symBuf, '\0');
    const std::string symbol(symBuf.begin(), nul);

    const int multiplier = symbol_scale::getPriceScale(symbol);
    if (multiplier == symbol_scale::kUnknown) {
        return std::nullopt;
    }

    // decimal -> double -> scaled INT32. llround recovers the exact scaled
    // integer despite double's inability to represent e.g. 1.10001 exactly
    // (1.10001 * 100000 = 110000.999... -> 110001). Returns nullopt if the
    // result won't fit in INT32 — we never quietly truncate into PriceData's
    // storage. (llround returns long long, so the comparison is done in 64-bit
    // before the narrowing cast.)
    const auto scale = [multiplier](double price) -> std::optional<std::int32_t> {
        const long long scaled = std::llround(price * multiplier);
        if (scaled < std::numeric_limits<std::int32_t>::min() ||
            scaled > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(scaled);
    };

    const std::optional<std::int32_t> bidScaled = scale(bid);
    const std::optional<std::int32_t> askScaled = scale(ask);
    if (!bidScaled || !askScaled) {
        return std::nullopt;
    }

    const auto timestamp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::microseconds(tsMicros)));

    return PriceData(*askScaled, *bidScaled, timestamp, symbol);
}

}  // namespace tick_packet
