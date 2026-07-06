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

import std;  // replaces <string>, <vector>, <array>
import symbolGroups;  // sweep::allSymbolsKnown — validates kSymbolGroups (see static_assert)

export namespace sweep {

// The DEFAULT symbols a sweep targets, used whenever the sweep itself doesn't
// override them (see resolveSymbolGroups below and e.g. kSymbolGroupsOverride
// in randomStrategySweep). Each entry becomes its OWN run (its own RUN_ID,
// strategy list and run descriptor). An entry may itself be a comma-separated
// list to evaluate multiple instruments inside a single run, e.g.
// "EURUSD,AUDUSD". So {"EURUSD,AUDUSD"} is one run over two instruments,
// while {"EURUSD", "AUDUSD"} is two independent runs. Edit and rebuild to change.
//
// constexpr std::array of string_views (not a runtime std::vector<std::string>)
// so the static_assert below can validate it against symbol_scale::kTable at
// compile time. std::to_array deduces the size from the list, so adding/removing
// an entry needs no hand-maintained count.
inline constexpr std::array kSymbolGroups = std::to_array<std::string_view>({
    "AUDUSD",       "EURUSD",        "GBRIDXGBP",   "GBPUSD",    "NZDUSD",
    "USDJPY",      "GBPJPY",        "EURJPY",      "USDCAD",    "FRAIDXEUR",
    "EURGBP",      "USA500IDXUSD",  "AUSIDXAUD",   "USDCHF",    "XAUUSD",
    "XAGUSD",      "USATECHIDXUSD", "EURCHF",      "DEUIDXEUR", "USA30IDXUSD",
    "LIGHTCMDUSD", "JPNIDXJPY",     "BRENTCMDUSD", "AUDNZD",    "EURAUD",
    "HKGIDXHKD",   "COPPERCMDUSD",  "USDSEK",      "EURNOK"});

// Compile-time guard: every symbol named in kSymbolGroups must exist in
// symbol_scale::kTable. The run side validates symbols against that same table
// (SqlManager::loadPriceData throws on an unknown one), so a typo here would
// still queue the run, only for the worker to reject it at runtime. Catch it
// here, at build time, before a wasted run reaches the queue.
static_assert(allSymbolsKnown(kSymbolGroups),
              "sweep::kSymbolGroups names a symbol missing from symbol_scale::kTable — "
              "add it to the table (symbolScale.cppm) or fix the typo");

// Resolve which symbol groups a sweep runs against: the sweep's own override
// when it set one (ParameterGenerator::setSymbolGroups), otherwise the full
// default kSymbolGroups. Returns owned strings so both sources come back as
// one type.
std::vector<std::string> resolveSymbolGroups(const std::vector<std::string>& sweepGroups) {
    if (!sweepGroups.empty()) {
        return sweepGroups;
    }
    return kSymbolGroups
        | std::views::transform([](std::string_view group) { return std::string(group); })
        | std::ranges::to<std::vector>();
}

// The run-level descriptor: what tick data to pull from QuestDB, plus the risk
// limits every strategy in the run runs under. Shared by every strategy in this
// run and linked to them by RUN_ID. SYMBOLS is one cleaned, comma-separated
// group from kSymbolGroups (see cleanSymbols in load/utility/symbolGroups.cppm).
tradingDefinitions::RunConfiguration makeRunConfiguration(const std::string& runId,
                                                          const std::string& symbols) {
    using namespace boost::decimal::literals;
    return tradingDefinitions::RunConfiguration{
        .RUN_ID = runId,
        .SYMBOLS = symbols,
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
