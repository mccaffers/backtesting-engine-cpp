// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <boost/decimal.hpp>

namespace decimal_convert {

// Converts a double into a decimal64_t via its shortest round-trip decimal
// string. Routing through text (rather than constructing from the binary double)
// stops clean decimals like 1.5 / 2.0 from snapping to an IEEE-754 neighbour,
// consistent with how decimal_json.hpp moves values through JSON. Feed it
// binary-exact values (halves, quarters) or explicit lists to keep this exact.
boost::decimal::decimal64_t toDecimal(double value);

}  // namespace decimal_convert
