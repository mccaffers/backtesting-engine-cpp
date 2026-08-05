// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// barStore — the shared per-symbol bar pipeline (OHLC and range bars).
//
// Historically each strategy built and owned its own bar histories inside
// during(), which ran AFTER decide() and left the run loop / live runner with
// no candles in scope for pre-decide checks (the ATR entry conditions). This
// store centralises that: the loop owner (backtest run or live worker)
// registers every timeframe anyone needs up front, feeds every tick in
// exactly once, and both the entry-condition gate and the strategies read
// from the same rolling windows.
//
// Design:
//  - A series is keyed by its bar duration. Registering the same duration
//    twice keeps the larger window — two consumers of one timeframe share
//    one history and each reads its own tail (std::span(...).last(n)).
//  - update() feeds the tick's ASK into every registered series, matching
//    ohlcBuilder's one historical consumer (OhlcBreakoutStrategy).
//  - The window `count` doubles as ohlcBuilder's prepopulateCount, so a
//    live worker's first tick warms every series from QuestDB
//    (OHLC_PREPOPULATE-gated) instead of starting cold.
//  - The last element of a series is always the in-progress bar; earlier
//    ones are complete (see ohlcBuilder).
//  - Range-bar series ride the same store and the same update(): identity is
//    a RangeBarSpec instead of a duration (registerRangeSeries/findRange),
//    state lives in rangebar::RangeSeries, and the first update per symbol
//    fires the raw-tick QuestDB warm-up exactly like the OHLC leg (see
//    rangeBarBuilder). One nuance vs OHLC: a range series' last element may
//    be just-closed, since completion is known at the breaching tick.
//
// NOT thread-safe: one instance per backtest run / per live worker, same
// ownership rule as TradeManager.
export module barStore;

import std;             // replaces <algorithm>, <chrono>, <cstddef>,
                        // <functional>, <stdexcept>, <string>, <string_view>,
                        // <unordered_map>, <vector>
import ohlcBuilder;     // ohlc::calculateOHLC
import ohlcObject;      // OhlcObject bar record
import priceData;       // PriceData
import rangeBarBuilder; // rangebar::RangeBarSpec / RangeSeries

export namespace bars {

// One bar series' shape: duration of a bar and the rolling window kept
// (also the QuestDB warm-up depth on a live cold start).
struct SeriesSpec {
    std::chrono::minutes minutes;
    int count;
};

class BarStore {
public:
    // Register a timeframe before the first update(). Registering a duration
    // twice keeps the larger count, so independent consumers (a strategy
    // timeframe and the ATR gate) can each declare what they need without
    // coordinating. Throws std::invalid_argument on a non-positive duration
    // or count — a misconfigured series should die loudly at setup, not
    // roll a bar per tick (see calculateOHLC's rationale).
    void registerSeries(const std::chrono::minutes minutes, const int count) {
        if (minutes < std::chrono::minutes{1} || count < 1) {
            throw std::invalid_argument(
                "BarStore::registerSeries: minutes and count must be >= 1");
        }
        for (SeriesSpec& spec : specs_) {
            if (spec.minutes == minutes) {
                spec.count = std::max(spec.count, count);
                return;
            }
        }
        specs_.push_back({minutes, count});
    }

    // Register a range-bar series before the first update(). Identity is
    // (atrTickWindow, atrPercent); re-registering an identity keeps the
    // larger count, mirroring registerSeries. Throws on any non-positive
    // field — same die-loudly-at-setup doctrine (RangeSeries would throw the
    // same on first tick, but per-strategy setup is where it belongs).
    void registerRangeSeries(const rangebar::RangeBarSpec& spec) {
        if (spec.atrTickWindow < 1 || spec.atrPercent < 1 || spec.count < 1) {
            throw std::invalid_argument(
                "BarStore::registerRangeSeries: all spec fields must be >= 1");
        }
        for (rangebar::RangeBarSpec& existing : rangeSpecs_) {
            if (rangebar::sameIdentity(existing, spec)) {
                existing.count = std::max(existing.count, spec.count);
                return;
            }
        }
        rangeSpecs_.push_back(spec);
    }

    // Feed the tick into EVERY registered series for its symbol. Call
    // exactly once per tick, unconditionally — bar history must never gap.
    // The loop owners call this BEFORE the entry gates and decide(), so the
    // ATR conditions and the strategies judge a tick against bar state that
    // already includes it.
    void update(const PriceData& tick) {
        if (!specs_.empty()) {
            // Heterogeneous find first: only a symbol's first tick pays for
            // the std::string key construction (same pattern as TradeManager).
            auto it = bySymbol_.find(std::string_view{tick.symbol});
            if (it == bySymbol_.end()) {
                it = bySymbol_.emplace(tick.symbol,
                                       std::vector<std::vector<OhlcObject>>(specs_.size()))
                         .first;
            }
            // A series registered after this symbol's first tick still gets a
            // (cold) history rather than an out-of-range index.
            if (it->second.size() < specs_.size()) {
                it->second.resize(specs_.size());
            }
            for (std::size_t i = 0; i < specs_.size(); ++i) {
                std::vector<OhlcObject>& series = it->second[i];
                ohlc::calculateOHLC(tick, tick.ask, specs_[i].minutes, series,
                                    specs_[i].count);
                // Rolling window: oldest bars dropped from the front.
                const auto cap = static_cast<std::size_t>(specs_[i].count);
                if (series.size() > cap) {
                    series.erase(series.begin(),
                                 series.end() - static_cast<std::ptrdiff_t>(cap));
                }
            }
        }
        if (!rangeSpecs_.empty()) {
            auto it = rangeBySymbol_.find(std::string_view{tick.symbol});
            if (it == rangeBySymbol_.end()) {
                it = rangeBySymbol_.emplace(tick.symbol,
                                            std::vector<rangebar::RangeSeries>{})
                         .first;
            }
            // Late-registered specs get a (cold) series appended, mirroring
            // the OHLC resize above; RangeSeries has no default ctor, so the
            // append is explicit rather than a resize.
            while (it->second.size() < rangeSpecs_.size()) {
                it->second.emplace_back(rangeSpecs_[it->second.size()]);
            }
            // A series' first update fires the QuestDB tick warm-up
            // (rangeBarBuilder), exactly like calculateOHLC's first-tick seed.
            for (rangebar::RangeSeries& series : it->second) {
                series.update(tick);
            }
        }
    }

    // The bars for (symbol, duration): chronological, last element
    // in-progress. nullptr when the duration was never registered or the
    // symbol has not ticked yet — callers treat both as "not warm".
    [[nodiscard]] const std::vector<OhlcObject>* find(
        const std::string_view symbol, const std::chrono::minutes minutes) const {
        std::size_t index = specs_.size();
        for (std::size_t i = 0; i < specs_.size(); ++i) {
            if (specs_[i].minutes == minutes) {
                index = i;
                break;
            }
        }
        if (index == specs_.size()) {
            return nullptr;
        }
        const auto it = bySymbol_.find(symbol);
        if (it == bySymbol_.end() || index >= it->second.size()) {
            return nullptr;
        }
        return &it->second[index];
    }

    // The range bars for (symbol, spec identity): chronological, last element
    // in-progress or just-closed (see rangeBarBuilder). count is ignored in
    // the lookup, like find() ignores it. nullptr when the identity was never
    // registered or the symbol has not ticked yet — callers treat both as
    // "not warm".
    [[nodiscard]] const std::vector<OhlcObject>* findRange(
        const std::string_view symbol, const rangebar::RangeBarSpec& spec) const {
        std::size_t index = rangeSpecs_.size();
        for (std::size_t i = 0; i < rangeSpecs_.size(); ++i) {
            if (rangebar::sameIdentity(rangeSpecs_[i], spec)) {
                index = i;
                break;
            }
        }
        if (index == rangeSpecs_.size()) {
            return nullptr;
        }
        const auto it = rangeBySymbol_.find(symbol);
        if (it == rangeBySymbol_.end() || index >= it->second.size()) {
            return nullptr;
        }
        return &it->second[index].bars();
    }

private:
    // Transparent hasher (same pattern as TradeManager::activeTrades) so the
    // per-tick find() takes a string_view and never allocates a temporary key.
    struct SymbolHash {
        using is_transparent = void;
        std::size_t operator()(const std::string_view symbol) const noexcept {
            return std::hash<std::string_view>{}(symbol);
        }
    };

    std::vector<SeriesSpec> specs_;
    // bySymbol_[symbol][i] is the history for specs_[i]. A handful of specs
    // per run, so the per-tick spec scans are trivially cheap.
    std::unordered_map<std::string, std::vector<std::vector<OhlcObject>>,
                       SymbolHash, std::equal_to<>>
        bySymbol_;
    // The range-bar leg, parallel in shape: rangeBySymbol_[symbol][i] is the
    // stateful series for rangeSpecs_[i] (bars plus the rolling tick window,
    // which is why the element is a RangeSeries rather than a bare vector).
    std::vector<rangebar::RangeBarSpec> rangeSpecs_;
    std::unordered_map<std::string, std::vector<rangebar::RangeSeries>,
                       SymbolHash, std::equal_to<>>
        rangeBySymbol_;
};

}  // namespace bars
