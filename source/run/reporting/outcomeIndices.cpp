// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/reporting/outcomeIndices.hpp"

#include "shared/utilities/env.hpp"

namespace outcome_index {

std::string weeklyIndex(const std::string_view base, const std::string_view batch) {
    if (batch.empty()) {
        return std::string{base};
    }
    return std::string{base} + "-" + std::string{batch};
}

std::string currentAlias(const std::string_view base) {
    return std::string{base} + "-current";
}

std::string isoWeekLabel(const std::time_t utc) {
    std::tm tm_buf{};
    gmtime_r(&utc, &tm_buf);
    // %G/%V are the ISO-8601 week-based year and week number (C99 strftime);
    // gmtime_r fills the tm_wday/tm_yday they derive from.
    char buf[16];
    std::strftime(buf, sizeof(buf), "%G-%V", &tm_buf);
    return std::string{buf};
}

std::string currentBatchLabel() {
    const std::string pinned = env::getOr("BACKTEST_BATCH", "");
    if (!pinned.empty()) {
        return pinned;
    }
    return isoWeekLabel(std::time(nullptr));
}

}  // namespace outcome_index
