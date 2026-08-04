// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// NyOpenRangeBreakoutStrategy — the New York open-range breakout: the hours
// before the US equities open define a coil; a break of that range shortly
// after the open trades the session's directional expansion. The direct
// sibling of SessionRangeBreakoutStrategy (the London variant), aimed at the
// instruments where the winning runs actually cluster — US index CFDs, gold,
// oil — whose liquidity event is the 09:30 New York open, not London's.
// Like the London strategy the range is anchored to the CLOCK, not a rolling
// bar count: the edge under test is specifically the open.
//
// One OHLC timeframe is read per symbol from the loop owner's shared BarStore
// (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the signal timeframe. The pre-open range is the
//                       highest high / lowest low of its CLOSED bars whose
//                       date (first-tick timestamp) falls in
//                       [nyOpen - RANGE_HOURS, nyOpen), padded by
//                       BUFFER_PIPS. Unlike the London strategy's fixed
//                       Asian window the range depth is SWEPT: 13 hours
//                       approximates the whole overnight session, 4 a tight
//                       pre-open coil — the data decides which definition
//                       carries the edge. Bars are first-tick anchored, so a
//                       bar STARTING inside the window may spill slightly
//                       past the open — accepted; the range is "bars that
//                       started before the open".
//
// Entry: only within ENTRY_WINDOW_MINUTES of the NY open (13:30 UTC under
// EDT, else 14:30 — market_hours::isNewYorkSummer, the same DST rule the
// session gate uses). The bid breaking above the padded pre-open high ->
// LONG; the ask breaking below the padded low -> SHORT. No trend filter: the
// range itself is the setup. Two structural refusals keep a partial range
// from trading: the series must reach back to the range start (else coverage
// is incomplete — first day of a run, or a window too small), and at least
// one closed bar must sit inside the range window.
//
// The entry cutoff lives HERE, not in the run loop's peakHoursOnly gate: the
// strategy must behave identically however the risk limits are configured.
// (When peakHoursOnly IS on, NewYork-mapped symbols align neatly — their
// permitted entries are exactly the 3 hours from the NY open, so windows
// beyond 180 minutes buy nothing.)
//
// Exits stay central (ATR-derived SL/TP enforced by Operations); the one
// strategy-driven exit is the optional time cap copied from
// SessionRangeBreakoutStrategy: when MAX_TRADE_DURATION_MINUTES > 0, during()
// closes the symbol's trade once open strictly longer than that, at the
// exit-side price — here it stops an open-drive entry riding into the
// afternoon drift.
//
// The same structural notes as the other strategies apply: the store updates
// BEFORE decide(), the strategy owns no bar state, and one instance sees
// every symbol interleaved. decide() deliberately scans the WHOLE series
// rather than a fixed tail — bars are selected by date, so a deeper window
// (the ATR gate may deepen a shared timeframe) is harmless. The signal
// re-fires while price holds beyond the range inside the entry window; the
// run loop's one-trade-per-symbol gate prevents stacking.

module;

#include "shared/tradingDefinitions/strategyConfig.hpp"

export module nyOpenRangeBreakoutStrategy;

import std;           // replaces <chrono>, <cstddef>, <cstdint>, <optional>,
                      // <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import marketHours;   // market_hours::isNewYorkSummer — the NY open rule
import symbolScale;   // symbol_scale::get — points-per-pip for the buffer

export class NyOpenRangeBreakoutStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframe / variables, window that cannot span
    // the range start -> entry cutoff) — a misconfigured run should die
    // loudly at construction, not trade silently wrong.
    explicit NyOpenRangeBreakoutStrategy(
        const tradingDefinitions::StrategyConfig& strategyConfig);

    std::optional<Direction> decide(const PriceData& tick,
                                    const bars::BarStore& barStore) override;

    // Strategy-driven exit hook: the optional time cap (see header). SL/TP
    // exits remain central.
    void during(const PriceData& price, const bars::BarStore& barStore,
                TradeManager& tradeManager) override;

private:
    // `{}` value-initializes: OHLCVariables is an aggregate of plain ints with
    // no defaults of its own, so without this the fields would hold
    // indeterminate values until the constructor body assigns them (reading
    // one before that is undefined behaviour). The constructor can't use a
    // member-init list here because it must validate the config first.
    tradingDefinitions::OHLCVariables signalCfg{};
    std::chrono::hours rangeHours{};
    std::int32_t bufferPips{};
    std::chrono::minutes entryWindow{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap
};

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
NyOpenRangeBreakoutStrategy::NyOpenRangeBreakoutStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.empty()) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: OHLC_VARIABLES needs one entry "
            "(the signal timeframe)");
    }

    signalCfg = ohlcVars[0];

    if (signalCfg.OHLC_MINUTES < 1) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: OHLC_MINUTES must be >= 1");
    }

    const auto& nyVars =
        strategyConfig.STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES;
    if (!nyVars) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: "
            "STRATEGY_VARIABLES.NY_OPEN_RANGE_BREAKOUT_VARIABLES is required "
            "(RANGE_HOURS, BUFFER_PIPS, ENTRY_WINDOW_MINUTES)");
    }
    rangeHours = std::chrono::hours{nyVars->RANGE_HOURS};
    bufferPips = nyVars->BUFFER_PIPS;
    entryWindow = std::chrono::minutes{nyVars->ENTRY_WINDOW_MINUTES};
    maxTradeDuration = std::chrono::minutes{nyVars->MAX_TRADE_DURATION_MINUTES};

    if (rangeHours < std::chrono::hours{1}) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: RANGE_HOURS must be >= 1");
    }
    // A range reaching back past the previous UTC midnight would cross the
    // weekend boundary on Mondays and mix Friday's US afternoon into "the
    // overnight" — cap the depth at the summer open (13:30, the earlier one)
    // so the range always starts on the same UTC day as the open it precedes.
    if (std::chrono::minutes{rangeHours} >
        std::chrono::hours{13} + std::chrono::minutes{30}) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: RANGE_HOURS must not reach past the "
            "previous UTC midnight (<= 13.5h, i.e. 13)");
    }
    if (bufferPips < 0) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: BUFFER_PIPS must be >= 0");
    }
    if (entryWindow < std::chrono::minutes{1}) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: ENTRY_WINDOW_MINUTES must be >= 1");
    }
    // The rolling window must reach from the range start (the coverage gate
    // decide() enforces) to the end of the entry window on a winter day
    // (14:30 open), with a two-bar margin so the range-start bar survives
    // eviction while the last decision tick is judged. A window that cannot
    // span that is a config that would NEVER trade — fail at construction
    // instead. Unlike the London strategy the requirement is anchored to the
    // OPEN, not midnight: range depth + entry window + margin.
    const std::int64_t spanMinutes =
        std::int64_t{signalCfg.OHLC_COUNT} * signalCfg.OHLC_MINUTES;
    const std::int64_t requiredMinutes =
        std::chrono::minutes{rangeHours}.count() + entryWindow.count() +
        std::int64_t{2} * signalCfg.OHLC_MINUTES;
    if (spanMinutes < requiredMinutes) {
        throw std::invalid_argument(
            "NyOpenRangeBreakoutStrategy: OHLC_COUNT x OHLC_MINUTES must cover "
            "the range through the entry window (>= RANGE_HOURS x 60 + "
            "ENTRY_WINDOW_MINUTES + 2 bars)");
    }
}

void NyOpenRangeBreakoutStrategy::during(const PriceData& price,
                                         const bars::BarStore& /*barStore*/,
                                         TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables. Only
    // the current tick's symbol is checked — each symbol's trade meets its
    // own next tick, which also supplies the right close price. Here it stops
    // an open-drive entry riding into the afternoon drift.
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

std::optional<Direction> NyOpenRangeBreakoutStrategy::decide(
    const PriceData& tick, const bars::BarStore& barStore) {
    using namespace std::chrono;

    // --- 1. THE CLOCK GATE ---
    // Entries only within the window after today's NY open; everything else
    // is refused before touching a bar. sys_days -> time_point is UTC
    // midnight, the anchor for both the open and the range window below.
    const sys_days day = floor<days>(tick.timestamp);
    const auto sinceMidnight = tick.timestamp - day;
    const minutes nyOpen =
        minutes{market_hours::isNewYorkSummer(day) ? hours{13} : hours{14}} +
        minutes{30};
    if (sinceMidnight < nyOpen || sinceMidnight >= nyOpen + entryWindow) {
        return std::nullopt;
    }

    // This symbol's bar history; nullptr means the timeframe was never
    // registered on the store or the symbol has not ticked yet. Fewer than
    // two bars cannot hold a closed bar (the last element is in-progress).
    const std::vector<OhlcObject>* signalSeries = barStore.find(
        tick.symbol, minutes{signalCfg.OHLC_MINUTES});
    if (signalSeries == nullptr || signalSeries->size() < 2) {
        return std::nullopt;
    }

    // --- 2. COVERAGE GATE ---
    // The series must reach back to the range start, or the pre-open window
    // is only partially represented (a run's first day, or eviction) and the
    // "range" would be a fragment — refuse to trade a partial coil.
    const system_clock::time_point openTime = sys_days{day} + nyOpen;
    const system_clock::time_point rangeStart = openTime - rangeHours;
    if (signalSeries->front().date > rangeStart) {
        return std::nullopt;
    }

    // --- 3. THE PRE-OPEN RANGE ---
    // Highest high / lowest low of the CLOSED bars that STARTED inside
    // [rangeStart, open) today. Selected by date rather than position, so a
    // deeper-than-OHLC_COUNT store window is harmless. The in-progress last
    // bar is excluded — during the entry window it is a post-open bar anyway.
    std::int32_t rangeHigh = std::numeric_limits<std::int32_t>::min();
    std::int32_t rangeLow = std::numeric_limits<std::int32_t>::max();
    bool rangeSeen = false;
    for (std::size_t i = 0; i + 1 < signalSeries->size(); ++i) {
        const OhlcObject& bar = (*signalSeries)[i];
        if (bar.date < rangeStart || bar.date >= openTime) {
            continue;
        }
        rangeHigh = std::max(rangeHigh, bar.high);
        rangeLow = std::min(rangeLow, bar.low);
        rangeSeen = true;
    }
    if (!rangeSeen) {
        return std::nullopt;
    }

    // --- 4. EXECUTION LOGIC ---
    // Pips scale UP to points (pips x pointsPerPip), the OhlcBreakoutStrategy
    // convention; an unknown symbol returns scale 0, which just disables the
    // buffer rather than corrupting the levels.
    const std::int32_t bufferPoints = bufferPips * symbol_scale::get(tick.symbol);
    if (tick.bid > rangeHigh + bufferPoints) {
        return Direction::LONG;
    }
    if (tick.ask < rangeLow - bufferPoints) {
        return Direction::SHORT;
    }
    return std::nullopt;
}
