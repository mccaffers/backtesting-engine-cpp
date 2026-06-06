// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "env.hpp"

#include <cstdlib>
#include <utility>

namespace env {

std::string getOr(const char* name, std::string fallback) {
    const char* val = std::getenv(name);
    return (val && *val) ? std::string{val} : std::move(fallback);
}

}  // namespace env
