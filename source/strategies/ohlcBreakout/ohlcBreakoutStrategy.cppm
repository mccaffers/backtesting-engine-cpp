// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// OhlcBreakoutStrategy — range breakout with an EMA trend filter.
//
// Two OHLC timeframes are built per symbol from the tick stream (both from the
// ask, matching the C# original):
//
//   OHLC_VARIABLES[0] — the breakout timeframe. The highest high / lowest low
//                       of its CLOSED candles (the in-progress bar is excluded)
//                       define the range, padded by BUFFER_PIPS.
//   OHLC_VARIABLES[1] — the trend timeframe. An EMA over its closes (period =
//                       half the candle count) is the macro trend filter.
//
// Entry: bid breaks above the range top in a macro uptrend -> LONG; ask breaks
// below the range bottom in a macro downtrend -> SHORT. Exits stay central
// (SL/TP pip distances enforced by Operations), so during() only builds bars.
//
// Two structural notes that differ from the C# framework:
//  - Bars are built in during(), not decide(): the run loop skips decide() for
//    a symbol while it has an open trade, but during() runs on every tick, so
//    the bar history never gaps. decide() therefore sees bars as of the
//    previous tick — immaterial, because the range comes from closed candles
//    only and is compared against the current tick's bid/ask.
//  - One strategy instance sees every symbol's ticks interleaved by timestamp
//    (SYMBOLS = "EURUSD,AUDUSD"), so all bar state is per-symbol, keyed by
//    tick.symbol — the C# "persistent list per instrument" requirement.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module ohlcBreakoutStrategy;

import std;           // replaces <algorithm>, <chrono>, <cstdint>, <functional>,
                      // <optional>, <stdexcept>, <string>, <string_view>,
                      // <unordered_map>, <vector>
import strategy;      // IStrategy base class
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import ohlcObject;    // OhlcObject bar record
import ohlcBuilder;   // ohlc::calculateOHLC
import ema;           // ema::calculate (integer EMA)
import symbolScale;   // symbol_scale::get — points-per-pip for the buffer

export class OhlcBreakoutStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframes / BUFFER_PIPS) — a misconfigured run
    // should die loudly at construction, not trade silently wrong.
    explicit OhlcBreakoutStrategy(const tradingDefinitions::StrategyConfig& strategyConfig);

    std::optional<Direction> decide(const PriceData& tick) override;

    // Builds the per-symbol bars every tick. TradeManager is unused — exits
    // are driven centrally by the configured SL/TP distances.
    void during(const PriceData& price, TradeManager& tradeManager) override;

private:
    struct SymbolState {
        std::vector<OhlcObject> breakoutBars;
        std::vector<OhlcObject> trendBars;
    };

    // Transparent hasher (same pattern as TradeManager::activeTrades) so the
    // per-tick find() takes a string_view and never allocates a temporary key.
    struct SymbolHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view symbol) const noexcept {
            return std::hash<std::string_view>{}(symbol);
        }
    };

    tradingDefinitions::OHLCVariables breakoutCfg;
    tradingDefinitions::OHLCVariables trendCfg;
    std::int32_t bufferPips;

    std::unordered_map<std::string, SymbolState, SymbolHash, std::equal_to<>> stateBySymbol;

    // Scratch buffers reused every decide() — cleared, never shrunk, so the
    // per-tick path stops allocating once their capacity settles.
    std::vector<std::int32_t> closesScratch;
    std::vector<std::int32_t> emaScratch;
};

namespace {

// Feed the tick into one timeframe's bars, then trim the history from the
// front to OHLC_COUNT — a rolling window, oldest bars dropped first. The last
// element is always the in-progress bar (see ohlcBuilder).
void updateBars(const PriceData& tick, const tradingDefinitions::OHLCVariables& cfg,
                std::vector<OhlcObject>& bars) {
    ohlc::calculateOHLC(tick, tick.ask, std::chrono::minutes{cfg.OHLC_MINUTES}, bars);
    const auto cap = static_cast<std::size_t>(cfg.OHLC_COUNT);
    if (bars.size() > cap) {
        bars.erase(bars.begin(), bars.end() - static_cast<std::ptrdiff_t>(cap));
    }
}

}  // namespace

OhlcBreakoutStrategy::OhlcBreakoutStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {
    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;
    if (ohlcVars.size() < 2) {
        throw std::invalid_argument(
            "OhlcBreakoutStrategy: OHLC_VARIABLES needs two entries "
            "(breakout timeframe, trend timeframe)");
    }
    breakoutCfg = ohlcVars[0];
    trendCfg = ohlcVars[1];
    for (const auto& cfg : {breakoutCfg, trendCfg}) {
        // COUNT >= 2 so the breakout list always has a closed candle besides
        // the in-progress one, and the trend EMA period (count/2) is >= 1.
        if (cfg.OHLC_COUNT < 2 || cfg.OHLC_MINUTES < 1) {
            throw std::invalid_argument(
                "OhlcBreakoutStrategy: OHLC_COUNT must be >= 2 and OHLC_MINUTES >= 1");
        }
    }
    const auto& breakoutVars = strategyConfig.STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES;
    if (!breakoutVars) {
        throw std::invalid_argument(
            "OhlcBreakoutStrategy: STRATEGY_VARIABLES.OHLC_BREAKOUT_VARIABLES is required "
            "(BUFFER_PIPS)");
    }
    bufferPips = breakoutVars->BUFFER_PIPS;
}

void OhlcBreakoutStrategy::during(const PriceData& price, TradeManager& /*tradeManager*/) {
    // This symbol's entry in stateBySymbol (its two bar histories), inserted
    // empty on the symbol's first tick. Heterogeneous find first: only that
    // first tick pays for the std::string key construction.
    auto stateIt = stateBySymbol.find(std::string_view{price.symbol});
    if (stateIt == stateBySymbol.end()) {
        stateIt = stateBySymbol.emplace(price.symbol, SymbolState{}).first;
    }
    updateBars(price, breakoutCfg, stateIt->second.breakoutBars);
    updateBars(price, trendCfg, stateIt->second.trendBars);
}

std::optional<Direction> OhlcBreakoutStrategy::decide(const PriceData& tick) {
    // This symbol's bar histories; absent means during() has not seen a tick
    // for the symbol yet, so there is nothing to decide on.
    const auto stateIt = stateBySymbol.find(std::string_view{tick.symbol});
    if (stateIt == stateBySymbol.end()) {
        return std::nullopt;
    }
    const SymbolState& state = stateIt->second;

    // Warm-up gate: no signals until both timeframes have a full window
    // (C#: `if (ohlcList.Count < totalOHLCCount) return`).
    if (state.breakoutBars.size() < static_cast<std::size_t>(breakoutCfg.OHLC_COUNT) ||
        state.trendBars.size() < static_cast<std::size_t>(trendCfg.OHLC_COUNT)) {
        return std::nullopt;
    }

    // No upper-bound check to pair with the gate above: updateBars trims each
    // history to OHLC_COUNT (oldest bars dropped from the front) on every
    // tick, so past the gate both windows hold exactly OHLC_COUNT bars.

    // --- 1. THE BREAKOUT LOGIC ---
    // Range from the CLOSED breakout candles (drop the in-progress last bar —
    // C#: closedCandles = Take(Count - 1)), padded by the pip buffer. Pips
    // scale UP to points here (pips * pointsPerPip); the C# divided because
    // its prices were raw decimals. An unknown symbol returns scale 0, which
    // just disables the buffer rather than corrupting the levels.
    std::int32_t highestHigh = std::numeric_limits<std::int32_t>::min();
    std::int32_t lowestLow = std::numeric_limits<std::int32_t>::max();
    for (auto bar = state.breakoutBars.begin(); bar != state.breakoutBars.end() - 1; ++bar) {
        highestHigh = std::max(highestHigh, bar->high);
        lowestLow = std::min(lowestLow, bar->low);
    }
    const std::int32_t bufferPoints = bufferPips * symbol_scale::get(tick.symbol);

    // --- 2. THE TREND FILTER LOGIC ---
    // EMA over the trend timeframe's closes (chronological, in-progress bar
    // included, like the C#). Period = half the window (C#: count * 0.5m,
    // truncated). Both the close and the EMA are read at the last index so
    // today's price is compared against today's moving average.
    closesScratch.clear();
    for (const auto& bar : state.trendBars) {
        closesScratch.push_back(bar.close);
    }
    const int emaPeriod = static_cast<int>(closesScratch.size() / 2);
    ema::calculate(closesScratch, emaPeriod, emaScratch);
    const std::int32_t currentClose = closesScratch.back();
    const std::int32_t currentEma = emaScratch.back();

    // --- 3. EXECUTION LOGIC ---
    // Re-fires while the condition holds once the position is closed; the run
    // loop's one-trade-per-symbol gate prevents stacking entries.
    if (tick.bid > highestHigh + bufferPoints && currentClose > currentEma) {
        return Direction::LONG;
    }
    if (tick.ask < lowestLow - bufferPoints && currentClose < currentEma) {
        return Direction::SHORT;
    }
    return std::nullopt;
}
