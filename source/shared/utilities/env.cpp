// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/utilities/env.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif

namespace env {

std::string getOr(const char* name, std::string fallback) {
    const char* val = std::getenv(name);
    return (val && *val) ? std::string{val} : std::move(fallback);
}

namespace {

// True when the variable name suggests its value is a secret that should not be
// printed verbatim. Matching is case-insensitive on the substrings below.
bool looksSensitive(std::string_view name) {
    static constexpr std::array markers{
        std::string_view{"PASSWORD"}, std::string_view{"PASSWD"},
        std::string_view{"SECRET"},   std::string_view{"TOKEN"},
        std::string_view{"CREDENTIAL"}, std::string_view{"KEY"},
        std::string_view{"AUTH"},
    };

    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    return std::ranges::any_of(markers, [&](std::string_view m) {
        return upper.find(m) != std::string_view::npos;
    });
}

// Masks a secret value, keeping a short prefix so it stays recognisable in logs.
// Short values are masked whole — 3 chars of a 5-char secret is most of it.
std::string maskValue(std::string_view value) {
    constexpr std::size_t reveal = 3;
    if (value.size() <= reveal * 2) return "********";
    return std::string{value.substr(0, reveal)} + "********";
}

}  // namespace

void printDiagnostics(int argc, const char* argv[]) {
    std::cerr << "=== diagnostics ===\n";
    std::cerr << "argv:";
    for (int i = 0; i < argc; ++i) std::cerr << ' ' << argv[i];
    std::cerr << '\n';

    // Collect and sort so the dump is stable run-to-run.
    std::vector<std::pair<std::string_view, std::string_view>> vars;
    for (char** ep = environ; ep && *ep; ++ep) {
        std::string_view entry{*ep};
        const auto eq = entry.find('=');
        const auto name = entry.substr(0, eq);
        const auto value = eq == std::string_view::npos
                               ? std::string_view{}
                               : entry.substr(eq + 1);
        vars.emplace_back(name, value);
    }
    std::ranges::sort(vars, {}, &std::pair<std::string_view, std::string_view>::first);

    std::cerr << "env (" << vars.size() << " vars):\n";
    for (const auto& [name, value] : vars) {
        std::cerr << "  " << name << '=';
        if (looksSensitive(name)) {
            std::cerr << maskValue(value);
        } else {
            std::cerr << value;
        }
        std::cerr << '\n';
    }
    std::cerr << "===================" << std::endl;
}

}  // namespace env
