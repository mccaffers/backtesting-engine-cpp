// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module parameterRange;

import std;

export namespace sweep {

// Abstract numeric range, the analogue of C#'s IParameterRange. A range knows
// how to enumerate the discrete values a single parameter should take.
class ParameterRange {
public:
    virtual ~ParameterRange() = default;
    [[nodiscard]] virtual std::vector<double> generateValues() const = 0;
};

// Inclusive [start, end] stepped by `step`, e.g. {80, 20, 140} -> 80,100,120,140.
// The epsilon absorbs floating point drift so the upper bound stays inclusive.
class LinearRange final : public ParameterRange {
public:
    LinearRange(const double start, const double step, const double end)
        : start_(start), step_(step), end_(end) {}

    [[nodiscard]] std::vector<double> generateValues() const override {
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

    [[nodiscard]] std::vector<double> generateValues() const override { return values_; }

private:
    std::vector<double> values_;
};

// base * multiplier^i for `iterations` values, e.g. {10, 2, 5} -> 10,20,40,80,160.
class ExponentialRange final : public ParameterRange {
public:
    ExponentialRange(double base, double multiplier, int iterations)
        : base_(base), multiplier_(multiplier), iterations_(iterations) {}

    [[nodiscard]] std::vector<double> generateValues() const override {
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

}  // namespace sweep
