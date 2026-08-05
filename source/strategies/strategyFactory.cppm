// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module strategyFactory;

import std;                  // replaces <array>, <memory>, <string_view>
import strategy;             // IStrategy
import randomStrategy;       // RandomStrategy
import ohlcBreakoutStrategy; // OhlcBreakoutStrategy
import fvgStrategy;          // FvgStrategy
import keltnerFadeStrategy;  // KeltnerFadeStrategy
import sessionRangeBreakoutStrategy; // SessionRangeBreakoutStrategy
import squeezeBreakoutStrategy; // SqueezeBreakoutStrategy
import nyOpenRangeBreakoutStrategy; // NyOpenRangeBreakoutStrategy
import liquiditySweepReversalStrategy; // LiquiditySweepReversalStrategy
import rangeVelocityStrategy; // RangeVelocityStrategy
import strategyErrors;       // UnknownStrategyError

export namespace strategies {

// The strategy names live trading considers active: winners are only pulled
// from the weekly winners index for these strategies (liveWinners), and each
// name here
// must have a branch in makeStrategy below.
inline constexpr std::array<std::string_view, 9> kActiveStrategies{
    "RandomStrategy", "OhlcBreakoutStrategy", "FvgStrategy",
    "KeltnerFadeStrategy", "SessionRangeBreakoutStrategy",
    "SqueezeBreakoutStrategy", "NyOpenRangeBreakoutStrategy",
    "LiquiditySweepReversalStrategy", "RangeVelocityStrategy"};

// Instantiate the strategy named by config.TRADING_VARIABLES.STRATEGY. Shared
// by the backtest path (Operations) and the live path (liveCommand), so adding
// a new strategy means adding one branch here; neither caller needs to know
// about the concrete type. Throws UnknownStrategyError on an unrecognised
// name; concrete constructors may throw std::invalid_argument on a malformed
// config (OhlcBreakoutStrategy validates its OHLC timeframes).
std::unique_ptr<IStrategy> makeStrategy(
    const tradingDefinitions::StrategyConfig& config) {
    const auto& name = config.TRADING_VARIABLES.STRATEGY;
    if (name == "RandomStrategy") {
        return std::make_unique<RandomStrategy>(config);
    }
    if (name == "OhlcBreakoutStrategy") {
        return std::make_unique<OhlcBreakoutStrategy>(config);
    }
    if (name == "FvgStrategy") {
        return std::make_unique<FvgStrategy>(config);
    }
    if (name == "KeltnerFadeStrategy") {
        return std::make_unique<KeltnerFadeStrategy>(config);
    }
    if (name == "SessionRangeBreakoutStrategy") {
        return std::make_unique<SessionRangeBreakoutStrategy>(config);
    }
    if (name == "SqueezeBreakoutStrategy") {
        return std::make_unique<SqueezeBreakoutStrategy>(config);
    }
    if (name == "NyOpenRangeBreakoutStrategy") {
        return std::make_unique<NyOpenRangeBreakoutStrategy>(config);
    }
    if (name == "LiquiditySweepReversalStrategy") {
        return std::make_unique<LiquiditySweepReversalStrategy>(config);
    }
    if (name == "RangeVelocityStrategy") {
        return std::make_unique<RangeVelocityStrategy>(config);
    }
    throw UnknownStrategyError(name);
}

}  // namespace strategies
