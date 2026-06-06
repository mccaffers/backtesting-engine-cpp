// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// Dedicated translation unit for the Boost.Redis implementation.
// Kept isolated because <boost/redis/src.hpp> transitively pulls in C-style
// <math.h>, which redefines signbit/isnan/isinf as macros on macOS and breaks
// any header in the same TU that uses std::signbit (e.g. nlohmann/json).
#include <boost/redis/src.hpp>
