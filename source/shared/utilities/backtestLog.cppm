// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// backtestLog (module) — the format-string half of the shared logging
// facility, layered on top of backtestLog.hpp. The header stays a plain
// GMF-safe include so non-module TUs can reach timestamp()/error()/
// set_quiet(); this module adds the std::format-based logLine that module
// TUs (ingest, load, live/*) share, replacing the per-command copies they
// used to carry.

module;

#include <cstdio>  // the `stdout` macro (import std exports std::fflush, not macros)

#include "shared/utilities/backtestLog.hpp"  // backtest_log::timestamp — GMF-safe

export module backtestLog;

import std;

export namespace backtest_log {

// Like std::println to stdout, but prefixed with the shared UTC millisecond
// timestamp (backtest_log::timestamp, so every command's lines match) and
// flushed immediately. stdout is fully buffered when redirected (pipe/file/
// service log), so without the flush infrequent lines (the once-a-minute
// reports) would sit in the buffer and only appear in a burst when the
// process exits.
template <class... Args>
void logLine(std::format_string<Args...> fmt, Args&&... args) {
    const std::string message = std::format(fmt, std::forward<Args>(args)...);
    std::println("{} {}", timestamp(), message);
    std::fflush(stdout);
    // No-op unless live mode registered the live-logs sink (backtestLog.hpp).
    shipToSink(false, message);
}

}  // namespace backtest_log
