// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <ctime>  // POSIX gmtime_r (not exported by `import std`)

#include "shared/utilities/backtestLog.hpp"

export module tradeManager;

import std;        // replaces <cstdint>, <unordered_map>, <string_view>, <vector>,
                   // <memory>, <algorithm>, <atomic>, <chrono>, <format>, <iomanip>,
                   // <iostream>, <sstream>
import trade;      // Trade, Direction
import priceData;  // PriceData

export class TradeManager {
private:
    // Transparent hasher so the per-tick lookups can probe with the tick's
    // symbol as a string_view — no temporary std::string per tick.
    struct SymbolHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view symbol) const noexcept {
            return std::hash<std::string_view>{}(symbol);
        }
    };
    // Open trades keyed by SYMBOL. The engine allows at most one open trade
    // per symbol (runTicks gates entries on hasActiveTradeForSymbol), so every
    // per-tick operation — mark-to-market, SL/TP review, the re-entry gate —
    // is a single O(1) find instead of a walk of the whole map.
    std::unordered_map<std::string, Trade, SymbolHash, std::equal_to<>> activeTrades;
    std::vector<Trade> closedTrades;
    // Trade ids only need to be unique within a run (reporting scopes them by
    // RUN_ID), so a plain per-manager counter replaces the old process-global
    // atomic that every worker thread contended on.
    std::uint64_t tradeCounter{0};
    // Running sums (int64 points-per-lot) maintained by openTrade/markToMarket/
    // closeTrade so the per-tick loss-limit check in runTicks stays O(1):
    // realized PnL across closed trades, and floating (mark-to-market) PnL
    // across open ones.
    std::int64_t closedPnl{0};
    std::int64_t openPnl{0};
    // Peak account equity (closedPnl + openPnl, int64 points-per-lot) and the
    // deepest peak-to-trough drop seen below it, sampled every time equity
    // moves (open/mark/close). This is the TRUE mark-to-market max drawdown —
    // it sees intra-trade floating losses, not just realized close-to-close.
    std::int64_t peakEquity{0};
    std::int64_t maxDrawdown{0};
    // Fold the current equity into the peak/drawdown extremes. Cheap integer
    // work; called from every mutation that changes equity so callers (and the
    // per-tick loop) need no extra bookkeeping.
    void updateDrawdown();

public:
    TradeManager() = default;
    std::string openTrade(const PriceData& tick,
                          std::int32_t size,
                          Direction direction,
                          std::int32_t stopDistancePips = 0,
                          std::int32_t limitDistancePips = 0);
    std::size_t reviewAccount() const;
    bool hasActiveTradeForSymbol(std::string_view symbol) const;
    // The symbol's open trade, or nullptr when it has none. O(1); the pointer
    // is invalidated by the next open/close.
    const Trade* findActiveTrade(std::string_view symbol) const;
    // Close the symbol's open trade. `liquidated` marks the close as forced by
    // the account loss limit rather than earned via SL/TP or strategy logic.
    bool closeTrade(std::string_view symbol,
                    std::int32_t closePrice,
                    const PriceData& tick,
                    bool liquidated = false);
    // Keyed by symbol (see activeTrades above).
    const std::unordered_map<std::string, Trade, SymbolHash, std::equal_to<>>&
    getActiveTrades() const;
    const std::vector<Trade>& getClosedTrades() const;
    // Realized PnL (int64 points-per-lot) across all closed trades. O(1).
    std::int64_t calculatePnl() const;
    // Floating (mark-to-market) PnL (int64 points-per-lot) across all open
    // trades, as of each trade's last marked price. O(1).
    std::int64_t unrealizedPnl() const;
    // Deepest peak-to-trough drop in account equity over the run, in int64
    // points-per-lot (>= 0). Includes intra-trade floating drawdown. Divide by
    // the symbol's points-per-pip for pips at the reporting boundary. O(1).
    std::int64_t maxDrawdownPoints() const;
    // Revalue open trades for this tick's symbol at its close-side price
    // (bid for LONG, ask for SHORT), updating their floating PnL.
    void markToMarket(const PriceData& tick);
    // Liquidate every open trade at its last marked price (timestamped with
    // `tick`), realizing the floating PnL — used when a run is cut off.
    void closeAllTrades(const PriceData& tick);
};

namespace {
// Floating PnL (int64 points-per-lot) of an open trade valued at `mark` — the
// same formula closeTrade uses for realized PnL, so liquidating at the last
// mark realizes exactly the floating amount. Pure integer: no decimal on the
// per-tick path. Divide by scalingFactor for pips at the reporting boundary.
std::int64_t floatingPnlAt(const Trade& trade, std::int32_t mark) {
    std::int32_t diff = mark - trade.entryPrice;
    if (trade.direction == Direction::SHORT) diff = -diff;
    return static_cast<std::int64_t>(diff) * trade.size;
}
}

void TradeManager::updateDrawdown() {
    const std::int64_t equity = closedPnl + openPnl;
    if (equity > peakEquity) peakEquity = equity;
    const std::int64_t drop = peakEquity - equity;
    if (drop > maxDrawdown) maxDrawdown = drop;
}

std::string TradeManager::openTrade(const PriceData& tick,
                                    std::int32_t size,
                                    Direction direction,
                                    std::int32_t stopDistancePips,
                                    std::int32_t limitDistancePips) {
    auto price = (direction == Direction::LONG) ? tick.ask : tick.bid;
    Trade trade(price, size, direction, tick.symbol);
    trade.openTime = tick.timestamp;  // simulation time, not wall clock
    trade.entryBid = tick.bid;
    trade.entryAsk = tick.ask;
    trade.id = std::format("T{}", tradeCounter++);
    trade.stopDistancePips = stopDistancePips;
    trade.limitDistancePips = limitDistancePips;
    trade.exitReferencePrice = (direction == Direction::LONG) ? tick.bid : tick.ask;
    // Precompute the SL/TP trigger prices once, so the per-tick exit check is a
    // pure integer comparison. Distances are in pips; convert to stored points
    // here (pips * points-per-pip) and anchor on exitReferencePrice (the
    // close-side of the entry spread).
    const std::int32_t stopOffset  = stopDistancePips  * trade.scalingFactor;
    const std::int32_t limitOffset = limitDistancePips * trade.scalingFactor;
    if (direction == Direction::LONG) {
        trade.stopPrice  = trade.exitReferencePrice - stopOffset;
        trade.limitPrice = trade.exitReferencePrice + limitOffset;
    } else {
        trade.stopPrice  = trade.exitReferencePrice + stopOffset;
        trade.limitPrice = trade.exitReferencePrice - limitOffset;
    }
    // Mark the trade at its entry tick: the close side of the spread. The
    // initial floating PnL is therefore the spread cost — true mark-to-market
    // equity dips by the spread the moment a trade opens.
    trade.lastMarkPrice = trade.exitReferencePrice;
    trade.floatingPnl = floatingPnlAt(trade, trade.lastMarkPrice);
    // try_emplace copy-constructs the key (pair::first) before it moves the
    // Trade into pair::second, so keying on trade.symbol here is safe. If the
    // symbol already has an open trade the insert is refused — accounting is
    // untouched and the existing trade's id comes back. Production never hits
    // that: runTicks gates entries on hasActiveTradeForSymbol.
    auto [it, inserted] = activeTrades.try_emplace(trade.symbol, std::move(trade));
    if (!inserted) {
        return it->second.id;
    }
    openPnl += it->second.floatingPnl;
    updateDrawdown();  // equity dips by the spread the moment a trade opens
    return it->second.id;
}

void TradeManager::markToMarket(const PriceData& tick) {
    const auto it = activeTrades.find(std::string_view{tick.symbol});
    if (it == activeTrades.end()) return;  // no position: equity unchanged
    Trade& trade = it->second;
    const auto mark = (trade.direction == Direction::LONG) ? tick.bid : tick.ask;
    const auto updated = floatingPnlAt(trade, mark);
    openPnl += updated - trade.floatingPnl;
    trade.floatingPnl = updated;
    trade.lastMarkPrice = mark;
    updateDrawdown();  // capture floating drawdown at this tick's mark
}

void TradeManager::closeAllTrades(const PriceData& tick) {
    // Snapshot symbols/prices first: closeTrade mutates activeTrades. Runs at
    // most once per run (loss-limit breach), so the allocation is off the
    // per-tick path.
    std::vector<std::pair<std::string, std::int32_t>> toClose;
    toClose.reserve(activeTrades.size());
    for (const auto& [symbol, trade] : activeTrades) {
        toClose.emplace_back(symbol, trade.lastMarkPrice);
    }
    for (const auto& [symbol, price] : toClose) {
        closeTrade(symbol, price, tick, /*liquidated=*/true);
    }
}

std::size_t TradeManager::reviewAccount() const {
    return activeTrades.size();
}

bool TradeManager::hasActiveTradeForSymbol(std::string_view symbol) const {
    return activeTrades.contains(symbol);
}

const Trade* TradeManager::findActiveTrade(std::string_view symbol) const {
    const auto it = activeTrades.find(symbol);
    return it != activeTrades.end() ? &it->second : nullptr;
}

bool TradeManager::closeTrade(std::string_view symbol,
                              std::int32_t closePrice,
                              const PriceData& tick,
                              bool liquidated) {
    auto it = activeTrades.find(symbol);
    if (it != activeTrades.end()) {
        // Move the trade out (ints survive the move; only the strings transfer)
        // so closing never copies the 5-string Trade struct.
        Trade closed = std::move(it->second);
        activeTrades.erase(it);
        closed.closePrice = closePrice;
        closed.closeTime = tick.timestamp;
        closed.liquidated = liquidated;
        std::int32_t diff = closePrice - closed.entryPrice;
        if (closed.direction == Direction::SHORT) diff = -diff;
        // Realized PnL in int64 points-per-lot (converted to pips at reporting).
        closed.pnl = static_cast<std::int64_t>(diff) * closed.size;
        closedPnl += closed.pnl;
        openPnl -= closed.floatingPnl;  // realized now, no longer floating
        closed.floatingPnl = 0;
        updateDrawdown();  // realized exit may differ from the last mark

        // Per-trade chatter is skipped under concurrent backtests (quiet),
        // which also avoids the formatting work below.
        if (!backtest_log::is_quiet()) {
            auto t = std::chrono::system_clock::to_time_t(tick.timestamp);
            std::tm utc{};
            gmtime_r(&t, &utc);
            std::ostringstream ts;
            ts << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");

            const char* side = (closed.direction == Direction::LONG) ? "BUY" : "SELL";
            // PnL is stored in points-per-lot; show it in pips for readability.
            const double pnlPips = closed.scalingFactor != 0
                ? static_cast<double>(closed.pnl) / closed.scalingFactor
                : 0.0;
            std::cout << ts.str()
                      << ", Trade Closed, " << closed.symbol
                      << ", " << side
                      << ", " << std::showpos << std::fixed << std::setprecision(2) << pnlPips
                      << std::noshowpos
                      << std::endl;
        }
        closedTrades.push_back(std::move(closed));
        return true;
    }
    return false;
}

const std::unordered_map<std::string, Trade, TradeManager::SymbolHash, std::equal_to<>>&
TradeManager::getActiveTrades() const {
    return activeTrades;
}

const std::vector<Trade>& TradeManager::getClosedTrades() const {
    return closedTrades;
}

std::int64_t TradeManager::calculatePnl() const {
    return closedPnl;
}

std::int64_t TradeManager::unrealizedPnl() const {
    return openPnl;
}

std::int64_t TradeManager::maxDrawdownPoints() const {
    return maxDrawdown;
}
