// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <stdexcept>
#include <string>
#include <utility>

// Thrown by the strategy factory when the configured strategy name does not
// match any known strategy. Derives from std::runtime_error so existing
// catch(const std::runtime_error&) / catch(const std::exception&) sites still
// match, while giving new callers a dedicated type to discriminate a
// configuration / domain error from any other runtime failure.
class UnknownStrategyError : public std::runtime_error {
public:
    explicit UnknownStrategyError(std::string name)
        : std::runtime_error("Unknown strategy: '" + name + "'"),
          name_(std::move(name)) {}

    const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
};
