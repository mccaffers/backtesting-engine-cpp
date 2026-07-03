// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// symbolGroups — tokenising and validating symbol groups.
//
// A "symbol group" is one comma-separated list of instruments evaluated inside
// a single run, e.g. "EURUSD,AUDUSD" (see sweep::kSymbolGroups in
// runConfigurationBuilder). Humans write groups with stray whitespace and empty
// fields; the run side (loadTicks in backtestRunner) splits SYMBOLS on ','
// WITHOUT trimming, so every group must be normalised before it reaches the RUN
// queue. One constexpr tokenizer (splitSymbols) backs both jobs:
//
//  - cleanSymbols()    : runtime normalisation for the RUN descriptor.
//  - allSymbolsKnown() : compile-time static_assert guard that every symbol a
//                        sweep names exists in symbol_scale::kTable — the run
//                        side rejects unknown symbols at runtime
//                        (SqlManager::loadPriceData throws), so catch the typo
//                        at build time, before a doomed run is queued.

export module symbolGroups;

import std;          // replaces <string>, <string_view>, <vector>, <span>
import symbolScale;  // symbol_scale::get / kUnknown — allSymbolsKnown lookup

export namespace sweep {

// Split one symbol group on ',' into its symbols: trim spaces/tabs around each
// field and drop empty ones ("EURUSD, AUDUSD," -> {"EURUSD", "AUDUSD"}). The
// returned views point into `group`, so they only live as long as its storage.
// constexpr so the static_assert guards below can run it at compile time
// (transient constexpr allocation — the vector never escapes the evaluation).
constexpr std::vector<std::string_view> splitSymbols(std::string_view group) {
    std::vector<std::string_view> symbols;
    for (std::size_t pos = 0; pos <= group.size();) {
        const std::size_t comma = group.find(',', pos);
        const std::size_t end =
            comma == std::string_view::npos ? group.size() : comma;
        std::string_view token = group.substr(pos, end - pos);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (!token.empty()) {
            symbols.push_back(token);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return symbols;
}

// Normalise one symbol group into a clean comma-separated string, ready for
// the RUN descriptor: "EURUSD, AUDUSD" must become "EURUSD,AUDUSD" or the run
// side's untrimmed lookup of " AUDUSD" fails.
std::string cleanSymbols(std::string_view group) {
    std::string result;
    for (const std::string_view symbol : splitSymbols(group)) {
        if (!result.empty()) {
            result += ',';
        }
        result += symbol;
    }
    return result;
}

// Compile-time guard for a symbol-group list: every symbol in every group must
// exist in symbol_scale::kTable. Used by the static_asserts on
// sweep::kSymbolGroups (runConfigurationBuilder) and inside
// ParameterGenerator::setSymbolGroups (covering every per-sweep override) so a
// typo'd symbol fails the build instead of queueing a run the worker rejects
// at runtime.
[[nodiscard]] constexpr bool allSymbolsKnown(
    std::span<const std::string_view> groups) {
    for (const std::string_view group : groups) {
        for (const std::string_view symbol : splitSymbols(group)) {
            if (symbol_scale::get(symbol) == symbol_scale::kUnknown) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace sweep
