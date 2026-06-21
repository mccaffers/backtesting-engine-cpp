// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <boost/decimal.hpp>

#include "shared/utilities/backtestLog.hpp"
#include "run/reporting/tradingResults.hpp"

export module resultsSummary;

import std;           // replaces <iostream>, <iomanip>, <cstddef>
import tradeManager;  // TradeManager
import trade;         // Trade, Direction
import symbolScale;   // symbol_scale::get — points-per-pip for the run's symbol

export class ResultsSummary {
public:
    static TradingResultsStats collect(const TradeManager& tradeManager,
                                       const tradingDefinitions::Configuration& config);
    static void summarise(const TradeManager& tradeManager,
                          const tradingDefinitions::Configuration& config);
};

namespace {

// Blend the per-trade aggregates into a single comparable performance score,
// ported from the C# reference. Three parts:
//   * expectancy/SQN  — per-trade edge, scaled by sqrt(trades) to reward
//                        frequency, then expressed against opening equity;
//   * Calmar          — CAGR over the (realized) max drawdown;
//   * confidence      — a multiplier that penalises a thin trades-per-year
//                        sample as statistical noise.
// Units: PnL is pip-denominated while STARTING_BALANCE is currency, so the
// percentages are pip-relative proxies — internally consistent for ranking,
// and the scaling constants (50/2.0, 50/3.0) are tunable. The maths runs in
// double (we need sqrt and the engine already casts decimal->double when
// reporting); results land back on the decimal stats fields.
//
// winPipSum / lossPipSum are the summed winning / losing trade PnL in pips
// (lossPipSum <= 0); maxDropPips is the true mark-to-market max drawdown of the
// equity curve, in pips (>= 0). Guards leave every score field at 0 for an
// unscoreable run (no closed trades, non-positive balance, no horizon).
void computePerformanceScore(TradingResultsStats& stats,
                             const tradingDefinitions::Configuration& config,
                             boost::decimal::decimal64_t winPipSum,
                             boost::decimal::decimal64_t lossPipSum,
                             boost::decimal::decimal64_t maxDropPips) {
    const double startingBalance = static_cast<double>(config.STARTING_BALANCE);
    if (stats.tradesClosed == 0 || startingBalance <= 0.0 || config.LAST_MONTHS <= 0) {
        return;  // unscoreable — leave all score fields at their 0 defaults
    }

    const double positive = static_cast<double>(stats.winners);
    const double negative = static_cast<double>(stats.losers);
    const double totalSum = positive + negative;
    if (totalSum == 0.0) {
        return;  // only breakeven trades — nothing to score
    }

    // Win rate: a run with no losers wins by definition; otherwise the share of
    // decisive trades that won.
    double winRate = 0.0;
    if (negative == 0.0) {
        winRate = 1.0;
    } else if (positive != 0.0) {
        winRate = positive / totalSum;
    }

    const double averageWin  = positive > 0.0 ? static_cast<double>(winPipSum) / positive : 0.0;
    const double averageLoss = negative > 0.0
        ? std::abs(static_cast<double>(lossPipSum) / negative) : 0.0;

    // Profit-to-loss weighting; the C# special cases pin the degenerate ends.
    double tradeRatio = 0.0;
    if (positive > 0.0 && negative > 0.0) {
        tradeRatio = (averageWin * positive) / (averageLoss * negative);
    }
    if (positive == 0.0) tradeRatio = 0.0;
    if (negative == 0.0) tradeRatio = 100.0;

    // Expectancy in pips, normalised to opening equity, then turned into a
    // Van Tharp SQN by the sqrt(trades) frequency factor and mapped so SQN 2.0
    // ~= score 50.
    const double expectancyPips    = averageWin * winRate - averageLoss * (1.0 - winRate);
    const double expectancyPercent = expectancyPips / startingBalance * 100.0;
    const double tradeFreqFactor   = std::sqrt(totalSum);
    const double systemQuality     = expectancyPercent * tradeFreqFactor;
    const double expectancyScore   = systemQuality * (50.0 / 2.0);

    // CAGR over the backtest horizon; finalPnl is already net profit in pips.
    const double years   = static_cast<double>(config.LAST_MONTHS) / 12.0;
    const double finalPnl = static_cast<double>(stats.finalPnl);
    const double cagrPercent = (finalPnl / startingBalance) / years * 100.0;

    // Realized max drawdown as a percent of opening equity, floored at 2% so a
    // near-flat curve cannot explode the Calmar ratio. Calmar 3.0 ~= score 50.
    const double maxDrawdownPercent = static_cast<double>(maxDropPips) / startingBalance * 100.0;
    const double adjustedDrawdown   = std::max(maxDrawdownPercent, 2.0);
    const double calmarScore        = (cagrPercent / adjustedDrawdown) * (50.0 / 3.0);

    // Trades-per-year confidence: too few decisive trades for the horizon is
    // noise, so we scale the whole score down rather than nudge it.
    double confidenceMultiplier = 0.1;
    if (totalSum >= 60.0 * years)      confidenceMultiplier = 1.0;
    else if (totalSum >= 40.0 * years) confidenceMultiplier = 0.9;
    else if (totalSum >= 20.0 * years) confidenceMultiplier = 0.8;
    else if (totalSum >= 10.0 * years) confidenceMultiplier = 0.6;
    else if (totalSum >= 5.0 * years)  confidenceMultiplier = 0.4;

    const double rawPerformance = expectancyScore * 0.5 + calmarScore * 0.5;
    const double performance    = rawPerformance * confidenceMultiplier;

    stats.winRate              = boost::decimal::decimal64_t{winRate};
    stats.tradeRatio           = boost::decimal::decimal64_t{tradeRatio};
    stats.expectancyScore      = boost::decimal::decimal64_t{expectancyScore};
    stats.calmarScore          = boost::decimal::decimal64_t{calmarScore};
    stats.confidenceMultiplier = boost::decimal::decimal64_t{confidenceMultiplier};
    stats.maxDrawdownPercent   = boost::decimal::decimal64_t{maxDrawdownPercent};
    stats.performanceScore     = boost::decimal::decimal64_t{performance};
}

}  // namespace

TradingResultsStats ResultsSummary::collect(const TradeManager& tradeManager,
                                            const tradingDefinitions::Configuration& config) {
    const auto& activeTrades = tradeManager.getActiveTrades();
    const auto& closedTrades = tradeManager.getClosedTrades();

    const std::size_t openedCount = activeTrades.size() + closedTrades.size();
    const std::size_t closedCount = closedTrades.size();

    std::size_t openedLong = 0;
    std::size_t openedShort = 0;
    for (const auto& [id, trade] : activeTrades) {
        if (trade.direction == Direction::LONG) ++openedLong;
        else ++openedShort;
    }

    std::size_t closedLong = 0;
    std::size_t closedShort = 0;
    std::size_t winners = 0;
    std::size_t losers = 0;
    std::size_t breakeven = 0;
    std::size_t liquidated = 0;
    boost::decimal::decimal64_t pnlSum{0};      // accumulated in pips
    boost::decimal::decimal64_t winPipSum{0};   // sum of winning trades, pips
    boost::decimal::decimal64_t lossPipSum{0};  // sum of losing trades, pips (<= 0)
    for (const auto& trade : closedTrades) {
        if (trade.direction == Direction::LONG) ++closedLong;
        else ++closedShort;
        if (trade.pnl > 0) ++winners;
        else if (trade.pnl < 0) ++losers;
        else ++breakeven;
        if (trade.liquidated) ++liquidated;
        // trade.pnl is int64 points-per-lot; convert to pips using the trade's
        // own points-per-pip so mixed-symbol runs sum correctly.
        if (trade.scalingFactor != 0) {
            const boost::decimal::decimal64_t pips =
                boost::decimal::decimal64_t{trade.pnl} / trade.scalingFactor;
            pnlSum += pips;
            if (trade.pnl > 0) winPipSum += pips;
            else if (trade.pnl < 0) lossPipSum += pips;
        }
    }

    // True mark-to-market max drawdown, tracked live in TradeManager (so it
    // sees intra-trade floating losses). It is an aggregate point sum, so it is
    // converted to pips with the run's primary symbol's points-per-pip — exact
    // for single-asset-class runs, matching the loss-limit floor convention in
    // Operations::run.
    const std::string primarySymbol =
        config.SYMBOLS.substr(0, config.SYMBOLS.find(','));
    const int primaryPointsPerPip = symbol_scale::get(primarySymbol);
    boost::decimal::decimal64_t maxDropPips{0};
    if (primaryPointsPerPip != 0) {
        maxDropPips = boost::decimal::decimal64_t{tradeManager.maxDrawdownPoints()}
                      / primaryPointsPerPip;
    }

    openedLong  += closedLong;
    openedShort += closedShort;

    TradingResultsStats stats;
    stats.finalPnl     = pnlSum;
    stats.tradesOpened = openedCount;
    stats.tradesClosed = closedCount;
    stats.openedLong   = openedLong;
    stats.openedShort  = openedShort;
    stats.closedLong   = closedLong;
    stats.closedShort  = closedShort;
    stats.winners      = winners;
    stats.losers       = losers;
    stats.breakeven    = breakeven;
    stats.liquidated   = liquidated;
    if (closedCount == 0) {
        stats.avgPnl = std::nullopt;
    } else {
        stats.avgPnl = pnlSum / boost::decimal::decimal64_t{static_cast<long long>(closedCount)};
    }

    computePerformanceScore(stats, config, winPipSum, lossPipSum, maxDropPips);
    return stats;
}

void ResultsSummary::summarise(const TradeManager& tradeManager,
                               const tradingDefinitions::Configuration& config) {
    // Per-strategy summary is skipped under concurrent backtests (quiet).
    if (backtest_log::is_quiet()) {
        return;
    }

    const auto stats = collect(tradeManager, config);

    std::cout << "Final PnL: " << std::fixed << std::setprecision(2) << stats.finalPnl << std::endl;
    std::cout << "Trades opened: " << stats.tradesOpened
              << "  (LONG: " << stats.openedLong << ", SHORT: " << stats.openedShort << ")" << std::endl;
    std::cout << "Trades closed: " << stats.tradesClosed
              << "  (LONG: " << stats.closedLong << ", SHORT: " << stats.closedShort << ")" << std::endl;
    std::cout << "Winners: " << stats.winners
              << "   Losers: " << stats.losers
              << "   Breakeven: " << stats.breakeven << std::endl;
    if (!stats.avgPnl) {
        std::cout << "Average PnL per closed trade: n/a (0 closed)" << std::endl;
    } else {
        std::cout << "Average PnL per closed trade: "
                  << std::fixed << std::setprecision(2) << *stats.avgPnl << std::endl;
    }
    std::cout << "Performance score: " << std::fixed << std::setprecision(2)
              << stats.performanceScore
              << "  (expectancy: " << stats.expectancyScore
              << ", calmar: " << stats.calmarScore
              << ", confidence: " << stats.confidenceMultiplier
              << ", maxDD%: " << stats.maxDrawdownPercent << ")" << std::endl;
}
