// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include <boost/decimal/literals.hpp>

#include "shared/tradingDefinitions/config/configuration.hpp"
#include "shared/utilities/queueKeys.hpp"

import rollingWindow;  // rolling::nextWindow, rolling::nextRunConfiguration,
                       // rolling::queueKeyFor

using namespace boost::decimal::literals;

// The ladder: each completed 3-month slice advances to the slice before it,
// until 9 months of history are covered, then one final full-history run.
TEST_CASE("nextWindow walks the 3-month slices back through 9 months", "[rollingWindow]") {
    const auto second = rolling::nextWindow(3, 0);
    REQUIRE(second.has_value());
    CHECK(second->lastMonths == 3);
    CHECK(second->offsetMonths == 3);

    const auto third = rolling::nextWindow(3, 3);
    REQUIRE(third.has_value());
    CHECK(third->lastMonths == 3);
    CHECK(third->offsetMonths == 6);
}

// The last slice reaches the full 9 months back, so it advances to the
// terminal run over the whole 9 months of data.
TEST_CASE("nextWindow ends the slices with the full-history run", "[rollingWindow]") {
    const auto full = rolling::nextWindow(3, 6);
    REQUIRE(full.has_value());
    CHECK(full->lastMonths == 9);
    CHECK(full->offsetMonths == 0);
}

// The full-history run is terminal — no more runs are queued after it.
TEST_CASE("nextWindow stops after the full-history run", "[rollingWindow]") {
    CHECK_FALSE(rolling::nextWindow(9, 0).has_value());
}

// Only windows exactly on the ladder chain: a hand-queued one-off sweep
// (whatever its window) must never start spawning follow-on runs.
TEST_CASE("nextWindow ignores off-ladder windows", "[rollingWindow]") {
    CHECK_FALSE(rolling::nextWindow(6, 0).has_value());
    CHECK_FALSE(rolling::nextWindow(12, 0).has_value());
    CHECK_FALSE(rolling::nextWindow(3, 1).has_value());
    CHECK_FALSE(rolling::nextWindow(9, 6).has_value());
    CHECK_FALSE(rolling::nextWindow(0, 0).has_value());
}

// The advancing strategy must be judged under identical rules in every
// window, so the descriptor carries the finished run's symbols, batch
// identity and every risk cap — only RUN_ID and the window itself change.
TEST_CASE("nextRunConfiguration carries symbols and risk caps to the next window", "[rollingWindow]") {
    tradingDefinitions::Configuration finished;
    finished.RUN_ID = "origin-run";
    finished.SYMBOLS = "EURUSD,AUDUSD";
    finished.BATCH = "2099-01";  // must chain — the terminal {9,0} run's docs
    finished.EXECUTION_TS = "2099-01-01T00:00:00Z";  // name the weekly index
    finished.LAST_MONTHS = 3;
    finished.OFFSET_MONTHS = 0;
    finished.STARTING_BALANCE = "25000"_dd;
    finished.MAX_LOSS_PERCENT = "7"_dd;
    finished.MAX_OPEN_TRADES = 2;
    finished.MAX_TRADES_PER_MINUTE = 30;
    finished.REPORT_FAILURES = false;
    finished.PEAK_HOURS_ONLY = true;  // non-default so a dropped copy fails
    finished.ENTRY_SLIPPAGE_TENTH_PIPS = 3;  // ditto — the stress must chain

    const auto next =
        rolling::nextRunConfiguration(finished, {3, 3}, "next-run");

    CHECK(next.RUN_ID == std::string("next-run"));
    CHECK(next.SYMBOLS == finished.SYMBOLS);
    CHECK(next.BATCH == finished.BATCH);
    CHECK(next.EXECUTION_TS == finished.EXECUTION_TS);
    CHECK(next.LAST_MONTHS == 3);
    CHECK(next.OFFSET_MONTHS == 3);
    CHECK(next.STARTING_BALANCE == finished.STARTING_BALANCE);
    CHECK(next.MAX_LOSS_PERCENT == finished.MAX_LOSS_PERCENT);
    CHECK(next.MAX_OPEN_TRADES == finished.MAX_OPEN_TRADES);
    CHECK(next.MAX_TRADES_PER_MINUTE == finished.MAX_TRADES_PER_MINUTE);
    CHECK(next.REPORT_FAILURES == finished.REPORT_FAILURES);
    CHECK(next.PEAK_HOURS_ONLY == finished.PEAK_HOURS_ONLY);
    CHECK(next.ENTRY_SLIPPAGE_TENTH_PIPS == finished.ENTRY_SLIPPAGE_TENTH_PIPS);
}

// Every rung of the ladder must itself be reachable from the sweep's seed
// window (3, 0) — a table typo that orphans a rung would silently truncate
// the chain, so walk it end to end.
TEST_CASE("the ladder chains from the seed window to the full-history run", "[rollingWindow]") {
    int last = 3, offset = 0;
    int steps = 0;
    while (const auto next = rolling::nextWindow(last, offset)) {
        last = next->lastMonths;
        offset = next->offsetMonths;
        ++steps;
        REQUIRE(steps <= 10);  // a cycle in the table must fail, not hang
    }
    CHECK(last == 9);
    CHECK(offset == 0);
    CHECK(steps == 3);
}

// Workers drain queue_keys::RUN_QUEUES in order, so wave ordering holds only
// if each chained window lands exactly one queue deeper than the window that
// spawned it. Walk the ladder and check the mapping rung by rung; by the end
// every chain queue must have been used (an unreachable queue would mean the
// ladder and the queue list drifted apart).
TEST_CASE("each chained window lands one chain queue deeper", "[rollingWindow]") {
    int last = 3, offset = 0;
    std::size_t depth = 0;
    while (const auto next = rolling::nextWindow(last, offset)) {
        ++depth;
        REQUIRE(depth < queue_keys::RUN_QUEUES.size());
        CHECK(rolling::queueKeyFor(*next) == queue_keys::RUN_QUEUES[depth]);
        last = next->lastMonths;
        offset = next->offsetMonths;
    }
    CHECK(depth == queue_keys::RUN_QUEUES.size() - 1);
}

// Only ladder rungs belong on the chain queues: the seed window and any
// hand-queued one-off go on the shared RUN queue, top priority alongside
// fresh grid sweeps.
TEST_CASE("seed and off-ladder windows map to the shared RUN queue", "[rollingWindow]") {
    CHECK(rolling::queueKeyFor({3, 0}) == queue_keys::RUN);
    CHECK(rolling::queueKeyFor({6, 0}) == queue_keys::RUN);
    CHECK(rolling::queueKeyFor({12, 0}) == queue_keys::RUN);
    CHECK(rolling::queueKeyFor({3, 1}) == queue_keys::RUN);
}

// The priority scan peeks each queue independently and LREMs the one a run
// was found on — two rungs sharing a key would break both the ordering and
// the retire.
TEST_CASE("the run queues are distinct keys", "[rollingWindow]") {
    const std::set<std::string> unique(queue_keys::RUN_QUEUES.begin(),
                                       queue_keys::RUN_QUEUES.end());
    CHECK(unique.size() == queue_keys::RUN_QUEUES.size());
}
