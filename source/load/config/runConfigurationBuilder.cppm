// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/decimal.hpp>
#include <boost/decimal/literals.hpp>

#include "run/reporting/elasticPublisher.hpp"  // elastic::nowIsoUtc
#include "run/reporting/outcomeIndices.hpp"    // outcome_index::currentBatchLabel
#include "shared/tradingDefinitions/config/runConfiguration.hpp"
#include "shared/utilities/env.hpp"

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

// The slippage stress toggle, read from the environment ONCE per load so the
// value is frozen into every queued run descriptor (results in Elasticsearch
// then record the stress they ran under — a worker-side env var would change
// the meaning of already-queued jobs). Tenths of a pip; unset/empty = 0 = off.
// Garbage or a negative value fails the load loudly — a stress sweep that
// silently ran unstressed is worse than no sweep.
int entrySlippageFromEnv() {
    const std::string raw = env::getOr("ENTRY_SLIPPAGE_TENTH_PIPS", "0");
    int tenthPips = 0;
    const auto [ptr, ec] =
        std::from_chars(raw.data(), raw.data() + raw.size(), tenthPips);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || tenthPips < 0) {
        throw std::invalid_argument(
            "ENTRY_SLIPPAGE_TENTH_PIPS must be a non-negative integer "
            "(tenths of a pip), got: " + raw);
    }
    return tenthPips;
}

// The batch identity a load stamps into every queued run descriptor: the
// weekly Elasticsearch index label plus the seed wall clock. Frozen ONCE per
// load invocation (same doctrine as entrySlippageFromEnv — the value must not
// change under already-queued jobs) and carried through Redis and every
// rolling-window rung, so the whole batch reports into one week's indices no
// matter when its runs drain. A default-constructed (empty) stamp is the
// legacy escape hatch: unsuffixed index names, no batch metadata.
struct BatchStamp {
    std::string label;        // "2026-28" — see outcome_index::isoWeekLabel
    std::string executionTs;  // ISO-8601 seed wall clock, shared by the batch
};

BatchStamp currentBatchStamp() {
    return {outcome_index::currentBatchLabel(), elastic::nowIsoUtc()};
}

// The run-level descriptor: what tick data to pull from QuestDB, plus the risk
// limits every strategy in the run runs under. Shared by every strategy in this
// run and linked to them by RUN_ID. SYMBOLS is one cleaned, comma-separated
// group from kSymbolGroups (see cleanSymbols in load/utility/symbolGroups.cppm).
// `batch` has no default on purpose: a new call site that forgot it would
// silently write every document to the unsuffixed fallback indices.
tradingDefinitions::RunConfiguration makeRunConfiguration(const std::string& runId,
                                                          const std::string& symbols,
                                                          const BatchStamp& batch) {
    using namespace boost::decimal::literals;
    return tradingDefinitions::RunConfiguration{
        .RUN_ID = runId,
        .SYMBOLS = symbols,
        .BATCH = batch.label,
        .EXECUTION_TS = batch.executionTs,
        // (3, 0) — the most recent 3 months — is the first rung of the
        // rolling-window ladder (rolling::nextWindow): each strategy that
        // completes this window is re-queued by the runner over the 3-month
        // slice before it, and so on through 9 months of history, ending with
        // one run over the full 9 months. OFFSET_MONTHS is how far back the
        // window ends (0 = the present day); a window pair NOT on the ladder
        // sweeps exactly once, with no follow-on runs.
        .LAST_MONTHS = 3,
        .OFFSET_MONTHS = 0,
        .STARTING_BALANCE = tradingDefinitions::DEFAULT_STARTING_BALANCE,
        // Cut a run off once it has lost 5% of the account (fail fast);
        // set <= 0 to run without any loss cutoff.
        .MAX_LOSS_PERCENT = 5_DD,
        // Cap on simultaneously open positions per run (<= 0 = unlimited).
        // The per-symbol gate already limits to one trade per symbol, so this
        // only bites on multi-symbol runs.
        .MAX_OPEN_TRADES = 1,
        // Cap on trade entries per sliding 60-second window of tick time
        // (<= 0 = unlimited) — a runaway-strategy brake.
        .MAX_TRADES_PER_MINUTE = 60,
        // Flip to false to silence liquidated runs from Elasticsearch once
        // sweeps scale up and loss-limit cutoffs are expected noise.
        .REPORT_FAILURES = false,
        // Entries only inside each symbol's peak session window (see
        // marketHours) — newly queued runs trade peak hours only; configs
        // persisted before the field existed parse as false.
        .PEAK_HOURS_ONLY = true,
        // Slippage stress: export ENTRY_SLIPPAGE_TENTH_PIPS=3 before `load`
        // to re-run the sweep with 0.3 pip of adverse entry slippage; the
        // rolling-window ladder carries the value through every rung.
        .ENTRY_SLIPPAGE_TENTH_PIPS = entrySlippageFromEnv(),
    };
}

}  // namespace sweep
