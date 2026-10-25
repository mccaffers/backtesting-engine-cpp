// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/utilities/base64.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace {

constexpr char B64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr auto buildDecodeTable() {
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 26; ++i) {
        table[static_cast<unsigned char>('A' + i)] = static_cast<int8_t>(i);
        table[static_cast<unsigned char>('a' + i)] = static_cast<int8_t>(26 + i);
    }
    for (int i = 0; i < 10; ++i) {
        table[static_cast<unsigned char>('0' + i)] = static_cast<int8_t>(52 + i);
    }
    table[static_cast<unsigned char>('+')] = 62;
    table[static_cast<unsigned char>('-')] = 62;  // URL-safe alternative
    table[static_cast<unsigned char>('/')] = 63;
    table[static_cast<unsigned char>('_')] = 63;  // URL-safe alternative
    return table;
}

constexpr auto B64decodeTable = buildDecodeTable();

}  // namespace

const std::string Base64::b64encode(const unsigned char* data, const size_t &len)
{
    if (data == nullptr || len == 0) return "";

    std::string result((len + 2) / 3 * 4, '=');
    const unsigned char *p = data;
    char *str = &result[0];
    size_t j = 0, pad = len % 3;
    const size_t last = len - pad;

    for (size_t i = 0; i < last; i += 3)
    {
        int n = int(p[i]) << 16 | int(p[i + 1]) << 8 | p[i + 2];
        str[j++] = B64chars[n >> 18];
        str[j++] = B64chars[n >> 12 & 0x3F];
        str[j++] = B64chars[n >> 6 & 0x3F];
        str[j++] = B64chars[n & 0x3F];
    }
    if (pad)  /// Set padding
    {
        int n = --pad ? int(p[last]) << 8 | p[last + 1] : p[last];
        str[j++] = B64chars[pad ? n >> 10 & 0x3F : n >> 2];
        str[j++] = B64chars[pad ? n >> 4 & 0x03F : n << 4 & 0x3F];
        str[j++] = pad ? B64chars[n << 2 & 0x3F] : '=';
    }
    return result;
}

const std::string Base64::b64decode(const unsigned char* data, const size_t &len)
{
    if (data == nullptr || len == 0) return "";

    if (len % 4 != 0) {
        throw std::invalid_argument(
            "Base64::b64decode: input length must be a multiple of 4 (got "
            + std::to_string(len) + ")");
    }

    size_t padCount = 0;
    if (data[len - 1] == '=') ++padCount;
    if (len >= 2 && data[len - 2] == '=') ++padCount;

    if (padCount > 2) {
        throw std::invalid_argument("Base64::b64decode: invalid padding");
    }

    const size_t outputSize = (len / 4) * 3 - padCount;
    std::string result;
    result.reserve(outputSize);

    const size_t numChunks = len / 4;
    for (size_t c = 0; c < numChunks; ++c) {
        const size_t i = c * 4;
        const unsigned char c0 = data[i];
        const unsigned char c1 = data[i + 1];
        const unsigned char c2 = data[i + 2];
        const unsigned char c3 = data[i + 3];

        const int8_t v0 = B64decodeTable[c0];
        const int8_t v1 = B64decodeTable[c1];

        if (v0 < 0 || v1 < 0) {
            throw std::invalid_argument("Base64::b64decode: invalid character");
        }

        if (c == numChunks - 1 && padCount > 0) {
            if (padCount == 2) {
                if (c2 != '=' || c3 != '=') {
                    throw std::invalid_argument(
                        "Base64::b64decode: invalid padding sequence");
                }
                result.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            } else {
                const int8_t v2 = B64decodeTable[c2];
                if (v2 < 0 || c3 != '=') {
                    throw std::invalid_argument(
                        "Base64::b64decode: invalid padding sequence or character");
                }
                result.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
                result.push_back(static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
            }
        } else {
            const int8_t v2 = B64decodeTable[c2];
            const int8_t v3 = B64decodeTable[c3];
            if (v2 < 0 || v3 < 0) {
                throw std::invalid_argument(
                    "Base64::b64decode: invalid character or mid-stream padding");
            }
            result.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            result.push_back(static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2)));
            result.push_back(static_cast<char>(((v2 & 0x03) << 6) | v3));
        }
    }

    return result;
}

std::string Base64::b64encode(const std::string& str)
{
    return b64encode(reinterpret_cast<const unsigned char*>(str.c_str()), str.size());
}

std::string Base64::b64decode(const std::string& str64)
{
    return b64decode(reinterpret_cast<const unsigned char*>(str64.c_str()), str64.size());
}
