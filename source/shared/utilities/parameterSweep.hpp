// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Parameter sweep generator: declare named numeric ranges and expand them into
// every combination (the cartesian product), so a strategy can be backtested
// across a grid of parameters in a single `load`.
//
// This is the C++ analogue of the C# StrategyParameterGenerator. C++ has no
// reflection, so a combination is returned keyed by parameter name
// (sweep::Combination) and the caller maps those values onto the strongly-typed
// Configuration fields (see source/commands/loadCommand.cpp).
namespace sweep {

// Abstract numeric range, the analogue of C#'s IParameterRange. A range knows
// how to enumerate the discrete values a single parameter should take.
class ParameterRange {
public:
    virtual ~ParameterRange() = default;
    virtual std::vector<double> generateValues() const = 0;
};

// Inclusive [start, end] stepped by `step`, e.g. {80, 20, 140} -> 80,100,120,140.
// The epsilon absorbs floating point drift so the upper bound stays inclusive.
class LinearRange final : public ParameterRange {
public:
    LinearRange(double start, double step, double end)
        : start_(start), step_(step), end_(end) {}

    std::vector<double> generateValues() const override {
        std::vector<double> values;
        if (step_ <= 0.0) {  // defensive: a non-positive step cannot advance
            values.push_back(start_);
            return values;
        }
        for (double value = start_; value <= end_ + 1e-9; value += step_) {
            values.push_back(value);
        }
        return values;
    }

private:
    double start_;
    double step_;
    double end_;
};

// An explicit set of values, e.g. {1, 3, 5, 8}, analogue of C#'s
// ExplicitParameterRange / AddParameterList.
class ExplicitRange final : public ParameterRange {
public:
    explicit ExplicitRange(std::vector<double> values)
        : values_(std::move(values)) {}

    std::vector<double> generateValues() const override { return values_; }

private:
    std::vector<double> values_;
};

// base * multiplier^i for `iterations` values, e.g. {10, 2, 5} -> 10,20,40,80,160.
class ExponentialRange final : public ParameterRange {
public:
    ExponentialRange(double base, double multiplier, int iterations)
        : base_(base), multiplier_(multiplier), iterations_(iterations) {}

    std::vector<double> generateValues() const override {
        std::vector<double> values;
        if (iterations_ > 0) {
            values.reserve(static_cast<std::size_t>(iterations_));
        }
        double current = base_;
        for (int i = 0; i < iterations_; ++i) {
            values.push_back(current);
            current *= multiplier_;
        }
        return values;
    }

private:
    double base_;
    double multiplier_;
    int iterations_;
};

// One point in the parameter grid: parameter name -> chosen value. Values are
// stored as doubles; integer parameters round-trip exactly, so getInt() is safe.
class Combination {
public:
    bool has(const std::string& name) const { return values_.count(name) > 0; }

    double get(const std::string& name) const {
        const auto it = values_.find(name);
        if (it == values_.end()) {
            throw std::out_of_range(
                "sweep::Combination: unknown parameter '" + name + "'");
        }
        return it->second;
    }

    int getInt(const std::string& name) const {
        return static_cast<int>(std::lround(get(name)));
    }

    const std::map<std::string, double>& values() const { return values_; }

private:
    friend class ParameterGenerator;
    std::map<std::string, double> values_;
};

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
                    next.values_[name] = value;
                    expanded.push_back(std::move(next));
                }
            }
            result = std::move(expanded);
        }
        return result;
    }

    std::size_t parameterCount() const { return ranges_.size(); }

private:
    // A vector (not a map) preserves registration order for reproducible output.
    std::vector<std::pair<std::string, std::unique_ptr<ParameterRange>>> ranges_;
};

}  // namespace sweep
