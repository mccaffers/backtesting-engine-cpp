// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

// Parameter sweep generator: declare named numeric ranges and expand them into
// every combination (the cartesian product), so a strategy can be backtested
// across a grid of parameters in a single `load`.
//
// This is the C++ analogue of the C# StrategyParameterGenerator. C++ has no
// reflection, so a combination is returned keyed by parameter name
// (sweep::Combination) and the caller maps those values onto the strongly-typed
// StrategyConfig fields (see sweep::makeStrategy in
// source/load/config/randomStrategy/makeStrategy.cppm).
export module parameterGenerator;

export import parameterRange;    // addRange() takes unique_ptr<ParameterRange>
export import sweepCombination;  // generateAllCombinations() returns Combination

import std;
import symbolGroups;  // allSymbolsKnown — setSymbolGroups' compile-time guard

export namespace sweep {

// Holds the registered ranges and expands them into every combination.
class ParameterGenerator {
public:
    // Register an arbitrary range under `name`. The convenience overloads below
    // mirror the C# Add* helpers and cover the common cases.
    ParameterGenerator& addRange(std::string name,
                                 std::unique_ptr<ParameterRange> range) {
        ranges_.emplace_back(std::move(name), std::move(range));
        return *this;
    }

    ParameterGenerator& addRange(std::string name, double start, double step,
                                 double end) {
        return addRange(std::move(name),
                        std::make_unique<LinearRange>(start, step, end));
    }

    ParameterGenerator& addList(std::string name, std::vector<double> values) {
        return addRange(std::move(name),
                        std::make_unique<ExplicitRange>(std::move(values)));
    }

    ParameterGenerator& addExponential(std::string name, double base,
                                       double multiplier, int iterations) {
        return addRange(
            std::move(name),
            std::make_unique<ExponentialRange>(base, multiplier, iterations));
    }

    // The symbol groups this sweep targets, set by the strategy sweep builder
    // (e.g. randomStrategySweep's kSymbolGroupsOverride). Empty means "no
    // override": the load command then falls back to the full default set —
    // see sweep::resolveSymbolGroups in runConfigurationBuilder. The groups
    // come in as a non-type template parameter (a reference to the caller's
    // constexpr array) so the guard below runs at compile time; they are
    // stored as owned strings so the generator doesn't hold views into caller
    // storage.
    template <const auto& Groups>
    ParameterGenerator& setSymbolGroups() {
        // Same compile-time guard as kSymbolGroups (runConfigurationBuilder):
        // a symbol missing from symbol_scale::kTable would queue a run the
        // worker then rejects at runtime (SqlManager::loadPriceData throws).
        // Checking here means every sweep that sets an override gets the
        // guard without repeating it.
        static_assert(allSymbolsKnown(Groups),
                      "setSymbolGroups: a group names a symbol missing from "
                      "symbol_scale::kTable — add it to the table "
                      "(symbolScale.cppm) or fix the typo");
        symbolGroups_.assign(Groups.begin(), Groups.end());
        return *this;
    }

    const std::vector<std::string>& symbolGroups() const { return symbolGroups_; }

    // Expand every registered range into the full cartesian product. Ranges are
    // expanded in registration order, so the last-registered parameter varies
    // fastest, keeping the output order stable and predictable.
    std::vector<Combination> generateAllCombinations() const {
        std::vector<Combination> result(1);  // seed with one empty combination
        for (const auto& [name, range] : ranges_) {
            const std::vector<double> values = range->generateValues();
            std::vector<Combination> expanded;
            expanded.reserve(result.size() * values.size());
            for (const Combination& base : result) {
                for (const double value : values) {
                    Combination next = base;
                    next.set(name, value);
                    expanded.push_back(std::move(next));
                }
            }
            result = std::move(expanded);
        }
        return result;
    }

    std::size_t parameterCount() const { return ranges_.size(); }

    // How many combinations the grid expands to — the product of every range's
    // value count — without materialising them. generateAllCombinations() is
    // O(product) in time and memory, so callers use this to surface the sweep
    // size (and get confirmation) while backing out is still free.
    std::size_t combinationCount() const {
        std::size_t count = 1;
        for (const auto& [name, range] : ranges_) {
            count *= range->generateValues().size();
        }
        return count;
    }

    // Lazily decode combination `index` of the cartesian product — the
    // counterpart of generateAllCombinations()[index] for grids too large to
    // expand. The product is read as a mixed-radix number over the ranges,
    // registration order most- to least-significant digit, so the
    // last-registered parameter varies fastest — the same order as the eager
    // expansion (pinned by the lazy-mirror test in tests/sweep.cpp).
    // Precondition: index < combinationCount().
    Combination combinationAt(std::size_t index) const {
        Combination combo;
        for (const auto& [name, range] : ranges_ | std::views::reverse) {
            const std::vector<double> values = range->generateValues();
            combo.set(name, values[index % values.size()]);
            index /= values.size();
        }
        return combo;
    }

    // Each range's name and value count, in registration order. Lazy consumers
    // (the sweep tests) derive axis strides for combinationAt from these, so a
    // parameter can be walked through all its values without materialising the
    // grid.
    std::vector<std::pair<std::string, std::size_t>> rangeValueCounts() const {
        std::vector<std::pair<std::string, std::size_t>> counts;
        counts.reserve(ranges_.size());
        for (const auto& [name, range] : ranges_) {
            counts.emplace_back(name, range->generateValues().size());
        }
        return counts;
    }

private:
    // A vector (not a map) preserves registration order for reproducible output.
    std::vector<std::pair<std::string, std::unique_ptr<ParameterRange>>> ranges_;

    // Symbol-group override (see setSymbolGroups); empty = use the default set.
    std::vector<std::string> symbolGroups_;
};

}  // namespace sweep
