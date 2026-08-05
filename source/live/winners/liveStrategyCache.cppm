// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveStrategyCache — turns the winning backtest runs (liveWinners) into
// runnable worker specs for the strategy runner: instantiates each winner's
// StrategyConfig through the shared factory, logging one cache line per
// strategy taken live and a skip line (with the reason) for any winner that
// cannot be traded. One bad historical config must not kill live startup —
// the caller only bails when NOTHING could be instantiated.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module liveStrategyCache;

import std;
import backtestLog;         // backtest_log::logLine
import barStore;            // bars::SeriesSpec — worker bar registrations
import entryConditions;     // conditions::gateSeriesFor — the ATR gate's series
import liveWinners;         // live::Winner
import liveStrategyRunner;  // live::WorkerSpec
import rangeBarBuilder;     // rangebar::RangeBarSpec — worker range registrations
import strategyFactory;     // strategies::makeStrategy
import symbolScale;         // symbol_scale::get — validates winner symbols

export namespace live {

class StrategyCache {
public:
    // Winner -> WorkerSpec, skipping (with a logged reason) winners whose
    // symbol is unknown to symbol_scale (the tick decoder drops such symbols,
    // so the worker would look cached but never receive a tick), whose config
    // has no UUID (the trade-lock identity — without one the lock key would
    // collide across every UUID-less strategy) or whose strategy constructor
    // rejects the config.
    static std::vector<WorkerSpec> build(const std::vector<Winner>& winners);
};

}  // namespace live

namespace live {

std::vector<WorkerSpec> StrategyCache::build(const std::vector<Winner>& winners) {
    std::vector<WorkerSpec> specs;
    specs.reserve(winners.size());
    for (const Winner& winner : winners) {
        if (symbol_scale::get(winner.symbol) == symbol_scale::kUnknown) {
            backtest_log::logLine("StrategyCache: skipping winner strategy={} "
                                  "symbol={} (runId={}) — symbol not in "
                                  "symbol_scale; no tick could route to it",
                                  winner.strategyName, winner.symbol, winner.runId);
            continue;
        }
        if (winner.config.UUID.empty()) {
            backtest_log::logLine("StrategyCache: skipping winner strategy={} "
                                  "symbol={} (runId={}) — config has no UUID",
                                  winner.strategyName, winner.symbol, winner.runId);
            continue;
        }
        try {
            // The worker's bar registrations: the strategy's OHLC timeframes
            // — {0,0} entries are the documented "builds no bars" sentinel
            // (RandomStrategy) and register nothing — plus the ATR entry
            // gate's series, always engaged in production.
            std::vector<bars::SeriesSpec> barSeries;
            for (const auto& ohlcVars : winner.config.OHLC_VARIABLES) {
                if (ohlcVars.OHLC_MINUTES >= 1 && ohlcVars.OHLC_COUNT >= 1) {
                    barSeries.push_back(
                        {std::chrono::minutes{ohlcVars.OHLC_MINUTES},
                         ohlcVars.OHLC_COUNT});
                }
            }
            // Range-bar series, same sentinel convention as the OHLC loop.
            // Winners written before RANGE_VARIABLES existed parse to an
            // empty vector and register nothing.
            std::vector<rangebar::RangeBarSpec> rangeSeries;
            for (const auto& rangeVars : winner.config.RANGE_VARIABLES) {
                if (rangeVars.RANGE_ATR_TICK_WINDOW >= 1 &&
                    rangeVars.RANGE_ATR_PERCENT >= 1 &&
                    rangeVars.RANGE_COUNT >= 1) {
                    rangeSeries.push_back(
                        {.atrTickWindow = rangeVars.RANGE_ATR_TICK_WINDOW,
                         .atrPercent = rangeVars.RANGE_ATR_PERCENT,
                         .count = rangeVars.RANGE_COUNT});
                }
            }
            WorkerSpec spec{
                .symbol = winner.symbol,
                .strategyName = winner.strategyName,
                .strategyUuid = winner.config.UUID,
                .vars = winner.config.TRADING_VARIABLES,
                .maxOpenTrades = winner.maxOpenTrades,
                .maxTradesPerMinute = winner.maxTradesPerMinute,
                .peakHoursOnly = winner.peakHoursOnly,
                .barSeries = std::move(barSeries),
                .rangeSeries = std::move(rangeSeries),
                .gateSeries = conditions::gateSeriesFor(winner.config),
                .strategy = strategies::makeStrategy(winner.config),
            };
            backtest_log::logLine(
                "StrategyCache: cached strategy={} uuid={} symbol={} "
                "score={:.2f} size={} stopAtr={} limitAtr={} "
                "maxOpen={} maxPerMin={} peakHours={} (runId={})",
                spec.strategyName, spec.strategyUuid, spec.symbol,
                winner.performanceScore, spec.vars.TRADING_SIZE,
                spec.vars.STOP_DISTANCE_IN_ATR,
                spec.vars.LIMIT_DISTANCE_IN_ATR, spec.maxOpenTrades,
                spec.maxTradesPerMinute, spec.peakHoursOnly, winner.runId);
            specs.push_back(std::move(spec));
        } catch (const std::exception& e) {
            backtest_log::logLine("StrategyCache: skipping winner strategy={} "
                                  "uuid={} symbol={} (runId={}) — {}",
                                  winner.strategyName, winner.config.UUID,
                                  winner.symbol, winner.runId, e.what());
        }
    }
    return specs;
}

}  // namespace live
