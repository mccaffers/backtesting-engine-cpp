// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/tradingDefinitions/config/configuration.hpp"
#include "shared/utilities/queueKeys.hpp"

export module rollingWindow;

import std;  // replaces <array>, <optional>, <string>

export namespace rolling {

// One backtest window: LAST_MONTHS long, ending offsetMonths before now —
// the same semantics as RunConfiguration's LAST_MONTHS/OFFSET_MONTHS.
struct Window {
    int lastMonths;
    int offsetMonths;

    bool operator==(const Window&) const = default;
};

// The ladder's terminal rung: one run over the full 9 months of history,
// ending at the present day. Exported because "proven over the full window"
// is also live's eligibility bar — the terminal run's results land in the
// dedicated winners index (elasticClient's resultsBaseFor) that liveWinners
// reads, so routing, ladder and live stay in lockstep if the ladder ever
// grows.
inline constexpr Window kFullHistory{9, 0};

// The rolling-window ladder a queue-sourced strategy walks after each
// completed backtest: the most recent 3 months, then each 3-month slice
// before it until 9 months of history are covered, then one final run over
// the full 9 months. Returns the window to queue next, or nullopt when the
// finished window is the terminal full-history run — or is not on the ladder
// at all, so a hand-queued one-off window can never start a chain.
std::optional<Window> nextWindow(int lastMonths, int offsetMonths);

// The run descriptor for a strategy advancing to `next`: a fresh RUN_ID and
// the next window, with SYMBOLS and every risk cap carried over from the
// finished run so the strategy is judged under identical rules in every
// window.
tradingDefinitions::RunConfiguration nextRunConfiguration(
    const tradingDefinitions::Configuration& finished,
    Window next,
    const std::string& newRunId);

// The Redis run queue a window's descriptor belongs on: rung N of the ladder
// lands on the N-th chain queue in queue_keys::RUN_QUEUES, so workers drain
// the whole grid (and every earlier rung) before touching it. Anything not
// produced by the ladder — the seed window, a hand-queued one-off — belongs
// on the shared RUN queue.
std::string queueKeyFor(Window window);

}  // namespace rolling

namespace {

struct Step {
    rolling::Window from;
    rolling::Window to;
};

// The ladder is an explicit table, not a formula, so only windows that are
// exactly on it chain — (3,6) is the last slice (it reaches the full 9 months
// back), so it advances to the terminal (9,0) full-history run, which appears
// on no left-hand side and therefore ends the chain.
constexpr std::array kLadder = std::to_array<Step>({
    {{3, 0}, {3, 3}},
    {{3, 3}, {3, 6}},
    {{3, 6}, rolling::kFullHistory},
});

// The queues are depth-indexed by rung, so the ladder and the queue list must
// grow together: RUN at index 0, then one chain queue per rung.
static_assert(kLadder.size() + 1 == queue_keys::RUN_QUEUES.size(),
              "one chain queue per ladder rung: extend queue_keys::RUN_QUEUES "
              "alongside kLadder");

}  // namespace

namespace rolling {

std::optional<Window> nextWindow(const int lastMonths, const int offsetMonths) {
    const Window finished{lastMonths, offsetMonths};
    for (const Step& step : kLadder) {
        if (step.from == finished) {
            return step.to;
        }
    }
    return std::nullopt;
}

std::string queueKeyFor(const Window window) {
    for (std::size_t rung = 0; rung < kLadder.size(); ++rung) {
        if (kLadder[rung].to == window) {
            return queue_keys::RUN_QUEUES[rung + 1];
        }
    }
    return queue_keys::RUN;
}

tradingDefinitions::RunConfiguration nextRunConfiguration(
    const tradingDefinitions::Configuration& finished,
    const Window next,
    const std::string& newRunId) {
    return tradingDefinitions::RunConfiguration{
        .RUN_ID = newRunId,
        .SYMBOLS = finished.SYMBOLS,
        // The batch identity must survive every rung: the terminal {9,0}
        // run's documents are what the weekly winners index exists for, and
        // dropping BATCH here would silently send them to the unsuffixed
        // fallback index instead.
        .BATCH = finished.BATCH,
        .EXECUTION_TS = finished.EXECUTION_TS,
        .LAST_MONTHS = next.lastMonths,
        .OFFSET_MONTHS = next.offsetMonths,
        .STARTING_BALANCE = finished.STARTING_BALANCE,
        .MAX_LOSS_PERCENT = finished.MAX_LOSS_PERCENT,
        .MAX_OPEN_TRADES = finished.MAX_OPEN_TRADES,
        .MAX_TRADES_PER_MINUTE = finished.MAX_TRADES_PER_MINUTE,
        .REPORT_FAILURES = finished.REPORT_FAILURES,
        .PEAK_HOURS_ONLY = finished.PEAK_HOURS_ONLY,
        .ENTRY_SLIPPAGE_TENTH_PIPS = finished.ENTRY_SLIPPAGE_TENTH_PIPS,
    };
}

}  // namespace rolling
