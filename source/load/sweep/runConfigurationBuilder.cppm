// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/decimal.hpp>
#include <boost/decimal/literals.hpp>

#include "shared/tradingDefinitions/config/runConfiguration.hpp"

export module runConfigurationBuilder;

import std;  // replaces <string>

export namespace sweep {

// The run-level descriptor: what tick data to pull from QuestDB, plus the risk
// limits every strategy in the sweep runs under. Shared by every strategy in
// this sweep and linked to them by RUN_ID.
tradingDefinitions::RunConfiguration makeRunConfiguration(const std::string& runId) {
    using namespace boost::decimal::literals;
    return tradingDefinitions::RunConfiguration{
        .RUN_ID = runId,
        .SYMBOLS = "EURUSD",
        .LAST_MONTHS = 6,
        .STARTING_BALANCE = tradingDefinitions::DEFAULT_STARTING_BALANCE,
        // Cut a run off once it has lost 5% of the account (fail fast);
        // set <= 0 to run without any loss cutoff.
        .MAX_LOSS_PERCENT = 5_DD,
        // Cap on simultaneously open positions per run (<= 0 = unlimited).
        // The per-symbol gate already limits to one trade per symbol, so this
        // only bites on multi-symbol runs.
        .MAX_OPEN_TRADES = 1,
        // Flip to false to silence liquidated runs from Elasticsearch once
        // sweeps scale up and loss-limit cutoffs are expected noise.
        .REPORT_FAILURES = true,
    };
}

}  // namespace sweep
