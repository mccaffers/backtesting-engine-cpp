// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module sweepCombination;

import std;

export namespace sweep {

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

    // Assign a parameter value; used by ParameterGenerator while expanding the
    // grid (module attachment rules rule out a cross-module friend here).
    void set(std::string name, double value) {
        values_[std::move(name)] = value;
    }

    const std::map<std::string, double>& values() const { return values_; }

private:
    std::map<std::string, double> values_;
};

}  // namespace sweep
