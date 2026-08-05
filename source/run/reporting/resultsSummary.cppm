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
    // Core overload for callers that have no Configuration in scope (runLoop's
    // performance gate). startingBalance and lastMonths feed the score's
    // normalisation/annualisation; primaryPointsPerPip converts the aggregate
    // max drawdown to pips (the Configuration overload derives it from the
    // run's primary symbol).
    static TradingResultsStats collect(const TradeManager& tradeManager,
                                       boost::decimal::decimal64_t startingBalance,
                                       int lastMonths,
                                       int primaryPointsPerPip);
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
                             const boost::decimal::decimal64_t startingBalanceDec,
                             const int lastMonths,
                             boost::decimal::decimal64_t winPipSum,
                             boost::decimal::decimal64_t lossPipSum,
                             boost::decimal::decimal64_t maxDropPips) {
    const double startingBalance = static_cast<double>(startingBalanceDec);
    if (stats.tradesClosed == 0 || startingBalance <= 0.0 || lastMonths <= 0) {
        return;  // unscoreable — leave all score fields at their 0 defaults
    }

    const double positive = static_cast<double>(stats.winners);
    const double negative = static_cast<double>(stats.losers);
    const double totalSum = positive + negative;
    if (totalSum == 0.0) {
        return;  // only breakeven trades — nothing to score
    }

    // Win rate over ALL closed trades (winners + losers + breakevens), so it
    // reads as "share of trades that won". A run of one winner and ninety-nine
    // breakevens is 1%, not the forced 100% the old no-losers special case
    // produced. Breakevens likewise weight the loss share below, so the
    // expectancy is a true per-trade mean.
    const double closedCount = static_cast<double>(stats.tradesClosed);
    const double winRate  = positive / closedCount;
    const double lossRate = negative / closedCount;

    const double averageWin  = positive > 0.0 ? static_cast<double>(winPipSum) / positive : 0.0;
    const double averageLoss = negative > 0.0
        ? std::abs(static_cast<double>(lossPipSum) / negative) : 0.0;

    // Profit-to-loss weighting; the C# special cases pin the degenerate ends.
    // averageLoss == 0 with losers present means the losing pips were
    // unmeasurable (unknown-scale trades) — pin to the cap rather than divide
    // to Inf, which nlohmann would silently serialise as null.
    double tradeRatio = 0.0;
    if (positive == 0.0) {
        tradeRatio = 0.0;
    } else if (negative == 0.0 || averageLoss == 0.0) {
        tradeRatio = 100.0;
    } else {
        tradeRatio = (averageWin * positive) / (averageLoss * negative);
    }

    // Expectancy in pips, normalised to opening equity, then turned into a
    // Van Tharp SQN by the sqrt(trades) frequency factor and mapped so SQN 2.0
    // ~= score 50.
    const double expectancyPips    = averageWin * winRate - averageLoss * lossRate;
    const double expectancyPercent = expectancyPips / startingBalance * 100.0;
    const double tradeFreqFactor   = std::sqrt(totalSum);
    const double systemQuality     = expectancyPercent * tradeFreqFactor;
    const double expectancyScore   = systemQuality * (50.0 / 2.0);

    // CAGR over the backtest horizon; finalPnl is already net profit in pips.
    const double years   = static_cast<double>(lastMonths) / 12.0;
    const double finalPnl = static_cast<double>(stats.finalPnl);
    const double cagrPercent = (finalPnl / startingBalance) / years * 100.0;

    // Max drawdown "percent": pip drawdown over the balance READ AS A PIP
    // BUDGET (the same convention as the loss floor in runLoop — there is no
    // pip-value model, so this is an internally consistent ranking proxy, not
    // a true percent of account value). Floored at 2% so a near-flat curve
    // cannot explode the Calmar ratio. Calmar 3.0 ~= score 50.
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

    // Belt-and-braces: a non-finite score must never reach the JSON layer,
    // where nlohmann silently serialises NaN/Inf as null and the run's metrics
    // vanish without an error. Substitute 0 and say so.
    const auto finiteOr0 = [](double v, const char* name) {
        if (std::isfinite(v)) return v;
        backtest_log::error(std::string("ResultsSummary: non-finite ") + name
                            + " replaced with 0");
        return 0.0;
    };

    stats.winRate              = boost::decimal::decimal64_t{finiteOr0(winRate, "winRate")};
    stats.tradeRatio           = boost::decimal::decimal64_t{finiteOr0(tradeRatio, "tradeRatio")};
    stats.expectancyScore      = boost::decimal::decimal64_t{finiteOr0(expectancyScore, "expectancyScore")};
    stats.calmarScore          = boost::decimal::decimal64_t{finiteOr0(calmarScore, "calmarScore")};
    stats.confidenceMultiplier = boost::decimal::decimal64_t{finiteOr0(confidenceMultiplier, "confidenceMultiplier")};
    stats.maxDrawdownPercent   = boost::decimal::decimal64_t{finiteOr0(maxDrawdownPercent, "maxDrawdownPercent")};
    stats.performanceScore     = boost::decimal::decimal64_t{finiteOr0(performance, "performanceScore")};
}

}  // namespace

TradingResultsStats ResultsSummary::collect(const TradeManager& tradeManager,
                                            const tradingDefinitions::Configuration& config) {
    const std::string primarySymbol =
        config.SYMBOLS.substr(0, config.SYMBOLS.find(','));
    return collect(tradeManager, config.STARTING_BALANCE, config.LAST_MONTHS,
                   symbol_scale::get(primarySymbol));
}

TradingResultsStats ResultsSummary::collect(const TradeManager& tradeManager,
                                            const boost::decimal::decimal64_t startingBalance,
                                            const int lastMonths,
                                            const int primaryPointsPerPip) {
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
    std::size_t unmeasured = 0;
    boost::decimal::decimal64_t pnlSum{0};      // accumulated in pips
    boost::decimal::decimal64_t winPipSum{0};   // sum of winning trades, pips
    boost::decimal::decimal64_t lossPipSum{0};  // sum of losing trades, pips (<= 0)
    for (const auto& trade : closedTrades) {
        if (trade.direction == Direction::LONG) ++closedLong;
        else ++closedShort;
        if (trade.liquidated) ++liquidated;
        // trade.pnl is int64 points-per-lot; convert to pips using the trade's
        // own points-per-pip so mixed-symbol runs sum correctly. A trade whose
        // symbol has no known scale (scalingFactor == 0) cannot be expressed in
        // pips, so it is excluded from the win/loss counters AND the pip sums —
        // classifying it while contributing zero pips would skew the averages
        // (and could zero averageLoss into a divide-by-Inf tradeRatio).
        if (trade.scalingFactor == 0) {
            ++unmeasured;
            continue;
        }
        if (trade.pnl > 0) ++winners;
        else if (trade.pnl < 0) ++losers;
        else ++breakeven;
        const boost::decimal::decimal64_t pips =
            boost::decimal::decimal64_t{trade.pnl} / trade.scalingFactor;
        pnlSum += pips;
        if (trade.pnl > 0) winPipSum += pips;
        else if (trade.pnl < 0) lossPipSum += pips;
    }
    if (unmeasured != 0) {
        backtest_log::error("ResultsSummary: " + std::to_string(unmeasured)
                            + " closed trade(s) on unknown-scale symbols excluded"
                              " from pip metrics");
    }

    // True mark-to-market max drawdown, tracked live in TradeManager (so it
    // sees intra-trade floating losses). It is an aggregate point sum, so it is
    // converted to pips with the run's primary symbol's points-per-pip — exact
    // for single-asset-class runs, matching the loss-limit floor convention in
    // Operations::run.
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

    computePerformanceScore(stats, startingBalance, lastMonths,
                            winPipSum, lossPipSum, maxDropPips);
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
