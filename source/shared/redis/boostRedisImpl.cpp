// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// Single translation unit dedicated to compiling the Boost.Redis (header-only)
// implementation. Boost.Redis pulls in Boost.Asio's SSL layer and OpenSSL,
// which leak C-style macros (Apple's signbit, OpenSSL namespace pollutants,
// Windows min/max). Confining src.hpp to its own object file keeps that blast
// radius away from nlohmann/json and the core business logic, and means the
// heavy C++20 coroutine/template machinery is parsed and compiled only once.
//
// Nothing else belongs here — no project headers, no JSON, no business logic.
#include <boost/redis/src.hpp>
