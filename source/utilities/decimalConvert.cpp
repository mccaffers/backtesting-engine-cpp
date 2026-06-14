// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "decimalConvert.hpp"

#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>

#include <boost/decimal/charconv.hpp>

namespace decimal_convert {

boost::decimal::decimal64_t toDecimal(double value) {
    std::array<char, 64> buffer;
    const auto [ptr, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::runtime_error("decimalConvert: to_chars failed for double");
    }
    boost::decimal::decimal64_t result;
    const auto parsed = boost::decimal::from_chars(buffer.data(), ptr, result);
    if (parsed.ec != std::errc{}) {
        throw std::runtime_error("decimalConvert: from_chars failed for double");
    }
    return result;
}

}  // namespace decimal_convert
