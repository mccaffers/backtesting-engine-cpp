// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// SessionRangeBreakoutStrategy — the classic London open-range breakout: the
// Asian session (00:00–06:00 UTC, matching market_hours' Asia window) defines
// a coil; a break of that range shortly after the London open trades the
// session's directional expansion. Unlike OhlcBreakoutStrategy the range is
// anchored to the CLOCK, not to a rolling bar count — the edge under test is
// specifically the open, so time is a first-class input here.
//
// One OHLC timeframe is read per symbol from the loop owner's shared BarStore
// (built from the ask — see barStore):
//
//   OHLC_VARIABLES[0] — the signal timeframe. Today's Asian range is the
//                       highest high / lowest low of its CLOSED bars whose
//                       date (first-tick timestamp) falls in [00:00, 06:00)
//                       UTC, padded by BUFFER_PIPS. Bars are first-tick
//                       anchored, so a bar STARTING inside the window may
//                       spill slightly past 06:00 — accepted; the range is
//                       "bars that started in the session".
//
// Entry: only within ENTRY_WINDOW_MINUTES of the London open (07:00 UTC under
// BST, else 08:00 — market_hours::isLondonSummer, the same DST rule the
// session gate uses). The bid breaking above the padded Asian high -> LONG;
// the ask breaking below the padded low -> SHORT. No trend filter: the range
// itself is the setup. Two structural refusals keep a partial range from
// trading: the series must reach back to before midnight (else today's Asian
// coverage is incomplete — first day of a run, or a window too small), and at
// least one closed bar must sit inside the Asian window.
//
// The entry cutoff lives HERE, not in the run loop's peakHoursOnly gate: the
// strategy must behave identically however the risk limits are configured.
// (When peakHoursOnly IS on, Europe-mapped symbols align neatly — their
// permitted entries are exactly the 3 hours from the London open.)
//
// Exits stay central (ATR-derived SL/TP enforced by Operations); the one
// strategy-driven exit is the optional time cap copied from
// OhlcBreakoutStrategy: when MAX_TRADE_DURATION_MINUTES > 0, during() closes
// the symbol's trade once open strictly longer than that, at the exit-side
// price — here it stops a London entry riding into New York chop.
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

export module sessionRangeBreakoutStrategy;

import std;           // replaces <chrono>, <cstddef>, <cstdint>, <optional>,
                      // <stdexcept>, <vector>
import strategy;      // IStrategy base class
import barStore;      // bars::BarStore — shared per-symbol bar histories
import priceData;     // PriceData
import trade;         // Direction
import tradeManager;  // TradeManager
import timeCapExit;   // strategy_exits::closeIfPastCap — the shared time cap
import ohlcObject;    // OhlcObject bar record
import marketHours;   // market_hours::isLondonSummer — the London open rule
import symbolScale;   // symbol_scale::get — points-per-pip for the buffer

export class SessionRangeBreakoutStrategy : public IStrategy {
public:
    // Validates the config up front and throws std::invalid_argument on a
    // malformed one (missing timeframe / variables, window that cannot span
    // midnight -> entry cutoff) — a misconfigured run should die loudly at
    // construction, not trade silently wrong.
    explicit SessionRangeBreakoutStrategy(
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
    std::int32_t bufferPips{};
    std::chrono::minutes entryWindow{};
    std::chrono::minutes maxTradeDuration{};  // <= 0 disables the time cap
};

namespace {
// The Asian session in UTC — MUST match market_hours' Asia window (00:00 to
// 06:00), which is the definition being traded against.
inline constexpr std::chrono::hours kAsiaSessionEnd{6};
// The latest possible London open (08:00 GMT); the ctor's window check uses
// the worst case so a winter run is as covered as a summer one.
inline constexpr std::chrono::minutes kLatestLondonOpen{std::chrono::hours{8}};
}  // namespace

// Config fields are assigned in the body, not a member-init list: the size
// check must run first to throw a descriptive error (an init list would have
// to use ohlcVars.at(0), dying with an unhelpful out_of_range instead). Their
// in-class {} initializers keep them defined in the meantime.
SessionRangeBreakoutStrategy::SessionRangeBreakoutStrategy(
    const tradingDefinitions::StrategyConfig& strategyConfig) {

    const auto& ohlcVars = strategyConfig.OHLC_VARIABLES;

    if (ohlcVars.empty()) {
        throw std::invalid_argument(
            "SessionRangeBreakoutStrategy: OHLC_VARIABLES needs one entry "
            "(the signal timeframe)");
    }

    signalCfg = ohlcVars[0];

    if (signalCfg.OHLC_MINUTES < 1) {
        throw std::invalid_argument(
            "SessionRangeBreakoutStrategy: OHLC_MINUTES must be >= 1");
    }

    const auto& sessionVars =
        strategyConfig.STRATEGY_VARIABLES.SESSION_RANGE_BREAKOUT_VARIABLES;
    if (!sessionVars) {
        throw std::invalid_argument(
            "SessionRangeBreakoutStrategy: "
            "STRATEGY_VARIABLES.SESSION_RANGE_BREAKOUT_VARIABLES is required "
            "(BUFFER_PIPS, ENTRY_WINDOW_MINUTES)");
    }
    bufferPips = sessionVars->BUFFER_PIPS;
    entryWindow = std::chrono::minutes{sessionVars->ENTRY_WINDOW_MINUTES};
    maxTradeDuration =
        std::chrono::minutes{sessionVars->MAX_TRADE_DURATION_MINUTES};

    if (bufferPips < 0) {
        throw std::invalid_argument(
            "SessionRangeBreakoutStrategy: BUFFER_PIPS must be >= 0");
    }
    if (entryWindow < std::chrono::minutes{1}) {
        throw std::invalid_argument(
            "SessionRangeBreakoutStrategy: ENTRY_WINDOW_MINUTES must be >= 1");
    }
    // The rolling window must reach from before midnight (the coverage gate
    // decide() enforces) to the end of the entry window on a winter day
    // (08:00 open), with a two-bar margin so the pre-midnight bar survives
    // eviction while the last decision tick is judged. A window that cannot
    // span that is a config that would NEVER trade — fail at construction
    // instead.
    const std::int64_t spanMinutes =
        std::int64_t{signalCfg.OHLC_COUNT} * signalCfg.OHLC_MINUTES;
    const std::int64_t requiredMinutes =
        kLatestLondonOpen.count() + entryWindow.count() +
        std::int64_t{2} * signalCfg.OHLC_MINUTES;
    if (spanMinutes < requiredMinutes) {
        throw std::invalid_argument(
            "SessionRangeBreakoutStrategy: OHLC_COUNT x OHLC_MINUTES must cover "
            "midnight through the entry window (>= 480 + ENTRY_WINDOW_MINUTES "
            "+ 2 bars)");
    }
}

void SessionRangeBreakoutStrategy::during(const PriceData& price,
                                          const bars::BarStore& /*barStore*/,
                                          TradeManager& tradeManager) {
    // Time cap (shared closeIfPastCap mechanics): close this symbol's trade
    // once open STRICTLY longer than maxTradeDuration; <= 0 disables. Only
    // the current tick's symbol is checked — each symbol's trade meets its
    // own next tick, which also supplies the right close price.
    strategy_exits::closeIfPastCap(price, tradeManager, maxTradeDuration);
}

std::optional<Direction> SessionRangeBreakoutStrategy::decide(
    const PriceData& tick, const bars::BarStore& barStore) {
    using namespace std::chrono;

    // --- 1. THE CLOCK GATE ---
    // Entries only within the window after today's London open; everything
    // else is refused before touching a bar. sys_days -> time_point is UTC
    // midnight, the anchor for both the open and the Asian window below.
    const sys_days day = floor<days>(tick.timestamp);
    const auto sinceMidnight = tick.timestamp - day;
    const minutes londonOpen{market_hours::isLondonSummer(day) ? hours{7}
                                                               : hours{8}};
    if (sinceMidnight < londonOpen || sinceMidnight >= londonOpen + entryWindow) {
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
    // The series must reach back past midnight, or today's Asian session is
    // only partially represented (a run's first day, or eviction) and the
    // "range" would be a fragment — refuse to trade a partial coil.
    const system_clock::time_point midnight = day;
    if (signalSeries->front().date > midnight) {
        return std::nullopt;
    }

    // --- 3. THE ASIAN RANGE ---
    // Highest high / lowest low of the CLOSED bars that STARTED inside
    // [00:00, 06:00) UTC today. Selected by date rather than position, so a
    // deeper-than-OHLC_COUNT store window is harmless. The in-progress last
    // bar is excluded — during the entry window it is a London bar anyway.
    const system_clock::time_point asiaEnd = midnight + kAsiaSessionEnd;
    std::int32_t asianHigh = std::numeric_limits<std::int32_t>::min();
    std::int32_t asianLow = std::numeric_limits<std::int32_t>::max();
    bool sessionSeen = false;
    for (std::size_t i = 0; i + 1 < signalSeries->size(); ++i) {
        const OhlcObject& bar = (*signalSeries)[i];
        if (bar.date < midnight || bar.date >= asiaEnd) {
            continue;
        }
        asianHigh = std::max(asianHigh, bar.high);
        asianLow = std::min(asianLow, bar.low);
        sessionSeen = true;
    }
    if (!sessionSeen) {
        return std::nullopt;
    }

    // --- 4. EXECUTION LOGIC ---
    // Pips scale UP to points (pips x pointsPerPip), the OhlcBreakoutStrategy
    // convention; an unknown symbol returns scale 0, which just disables the
    // buffer rather than corrupting the levels.
    const std::int32_t bufferPoints = bufferPips * symbol_scale::get(tick.symbol);
    if (tick.bid > asianHigh + bufferPoints) {
        return Direction::LONG;
    }
    if (tick.ask < asianLow - bufferPoints) {
        return Direction::SHORT;
    }
    return std::nullopt;
}
