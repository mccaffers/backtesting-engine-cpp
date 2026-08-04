// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveStrategyRunner — executes the cached winning strategies against the
// live tick stream.
//
// Threading model: one dedicated worker thread per live strategy instance,
// each with its own tick queue. The UdpReceiver thread only routes (enqueue +
// notify), so a slow strategy can never stall the socket; and because a
// strategy instance is only ever touched by its own worker, the stateful
// strategies (OhlcBreakout's per-symbol bars, Random's RNG) need no locking
// and see ticks in arrival order. The shared ThreadPool is deliberately NOT
// used here — pooled submits could run the same strategy instance
// concurrently for back-to-back ticks.
//
// Per tick a worker runs only the live-relevant slice of trading::runTicks
// (runLoop.cppm): book sync (broker positions -> TradeManager, throttled to
// bookSyncInterval) -> decide -> caps (MAX_TRADES_PER_MINUTE sliding window,
// MAX_OPEN_TRADES against the broker position count) -> gate -> emit order,
// then during, then the close diff (a strategy that closed a book trade in
// during() emits a CloseIntent). No markToMarket, no reviewStopAndLimit, no
// closeAllTrades — the broker owns exits; the book mirrors the broker.

module;

#include "shared/tradingDefinitions/variables/tradingVariables.hpp"
#include "shared/utilities/backtestLog.hpp"

export module liveStrategyRunner;

import std;          // replaces <atomic>, <condition_variable>, <deque>,
                     // <functional>, <optional>, <thread>, <unordered_map>,
                     // <vector>
import barStore;         // bars::BarStore — the worker's shared bar pipeline
import entryConditions;  // conditions::check — pre-decide ATR gate
import rangeBarBuilder;  // rangebar::RangeBarSpec — range-bar registrations
import liveTrace;    // live_trace::emit — live-traces documents
import marketHours;  // market_hours::tradePermitted
import priceData;    // PriceData
import strategy;     // IStrategy
import trade;        // Direction
import tradeManager; // TradeManager

export namespace live {

// One order signal, exactly what phase 1 logs and what a phase-2 broker
// channel will consume. Prices are the scaled fixed-point points the decoder
// produced (see tickPacket / symbolScale).
struct OrderIntent {
    std::string strategyName;
    std::string strategyUuid;
    std::string symbol;
    Direction direction;
    std::int32_t size;
    std::int32_t stopDistancePips;
    std::int32_t limitDistancePips;
    std::int32_t bid;
    std::int32_t ask;
    std::chrono::system_clock::time_point timestamp;  // tick timestamp
};

// Entry gate: true = trading may proceed. Injected as a functor so unit tests
// need no Redis and phase 2 can layer more gates behind one seam. Called on a
// worker thread, only when a strategy signals.
using TradeGate = std::function<bool(const std::string& strategyUuid,
                                     const std::string& direction)>;
using OrderSink = std::function<void(const OrderIntent&)>;

// Broker position count for one strategy UUID (the PL# list a separate
// producer refreshes from the broker every ~2 minutes — see
// redis_positions::PositionManager). nullopt = UNKNOWN (Redis unreachable):
// the MAX_OPEN_TRADES check then fails closed, same doctrine as the trade
// lock. Called on a worker thread, only when a strategy signals AND its spec
// caps open trades.
using PositionCounter =
    std::function<std::optional<int>(const std::string& strategyUuid)>;

// One broker deal for one worker's symbol, decoded from the PL#/PO# book
// (see redisPositionFeed). The side-map entry the close path addresses.
struct BookedPosition {
    std::string dealId;         // EMPTY until the deal confirmed — a close
                                // cannot be sent without it
    std::string dealReference;  // the PO#/PL# book key
    Direction direction{Direction::LONG};
    double brokerSize{};        // exact broker units — what a close must send
    std::int32_t engineSize{};  // brokerSize / sizeModifier, for the book Trade
    std::int32_t level{};       // booked entry price, scaled INT32 points
    std::chrono::system_clock::time_point openedAt;
};

// Broker positions for (strategy UUID, symbol). nullopt = UNKNOWN (Redis
// unreachable): the book keeps its last known state — FAIL-OPEN, unlike the
// entry gates, because a stale book can at worst cause a redundant close
// (absorbed by the channel's blank-dealId guard, its recently-closed window
// and the broker itself) while entries stay fail-closed via the caps/locks.
// Called on a worker thread, at most once per bookSyncInterval.
using PositionFeed = std::function<std::optional<std::vector<BookedPosition>>(
    const std::string& strategyUuid, const std::string& symbol)>;

// Mirror of OrderIntent for strategy-initiated closes: emitted when a
// strategy's during() closed a book trade, carrying the broker identity the
// order channel needs.
struct CloseIntent {
    std::string strategyName;
    std::string strategyUuid;
    std::string symbol;
    Direction direction{Direction::LONG};  // the OPEN position's direction
    double brokerSize{};
    std::string dealId;
    std::string dealReference;
    std::chrono::system_clock::time_point timestamp;  // closing tick
};
using CloseSink = std::function<void(const CloseIntent&)>;

// One live strategy instance bound to ONE symbol (multi-symbol backtest
// configs were split per symbol during winner selection). Instances sharing a
// UUID also share their lock keys per direction — matching the C# TradeLocks
// semantics.
struct WorkerSpec {
    std::string symbol;
    std::string strategyName;
    std::string strategyUuid;
    tradingDefinitions::TradingVariables vars;  // size / stop / limit for orders
    // Risk caps from the winning run's config; defaults mirror
    // tradingDefinitions::RunConfiguration (<= 0 disables a cap, and the
    // per-minute brake is ON by default there too). Both are per strategy
    // UUID: a UUID trading several symbols shares one budget, exactly like
    // MAX_OPEN_TRADES / MAX_TRADES_PER_MINUTE cap a whole backtest run.
    int maxOpenTrades{0};
    int maxTradesPerMinute{60};
    // Peak-market-hours entry filter (market_hours::tradePermitted), from
    // the winning run's config like the caps above: out-of-session ticks
    // skip decide() entirely. Book sync and during()/close-diff are never
    // gated — the broker owns exits.
    bool peakHoursOnly{false};
    // Bar series to register on the worker's BarStore: the strategy's OHLC
    // timeframes (StrategyCache skips the {0,0} "no bars" sentinels) so a
    // bar-reading strategy's decide() has history to work from.
    std::vector<bars::SeriesSpec> barSeries;
    // Range-bar series, same contract as barSeries (StrategyCache skips the
    // all-zeros sentinels). Default-empty: pre-range winners and test specs
    // register nothing and behave exactly as before.
    std::vector<rangebar::RangeBarSpec> rangeSeries;
    // The ATR entry conditions' series (conditions::gateSeriesFor). Engaged
    // in production by StrategyCache; tests that script caps/gate/sink in
    // isolation leave it disengaged, and the vars then flow into OrderIntent
    // as literal pip distances.
    std::optional<bars::SeriesSpec> gateSeries;
    std::unique_ptr<IStrategy> strategy;
};

// Counters are cumulative since start(); routed counts per-worker deliveries
// (one tick fanned out to two workers counts twice).
struct RunnerStats {
    std::uint64_t routed{};
    std::uint64_t ignoredSymbol{};  // decoded ticks with no worker for their symbol
    std::uint64_t queueDropped{};   // oldest-tick drops on queue overflow
    std::uint64_t sessionSkipped{}; // peakHoursOnly: out-of-session ticks
                                    // (decide() skipped)
    std::uint64_t conditionsSkipped{}; // ATR entry conditions failed: gate
                                       // series not warm, spread too wide vs
                                       // ATR, or volatility below the floors
                                       // (decide() skipped)
    std::uint64_t signals{};        // decide() returned a direction
    std::uint64_t rateBlocked{};      // MAX_TRADES_PER_MINUTE window was full
    std::uint64_t positionBlocked{};  // at MAX_OPEN_TRADES, or count unknown
    std::uint64_t lockBlocked{};    // gate refused (lock held, or fail-closed)
    std::uint64_t ordersLogged{};   // OrderIntents handed to the sink
    std::uint64_t bookSeeded{};     // broker deals seeded into a worker's book
    std::uint64_t bookRemoved{};    // book trades dropped (closed at broker)
    std::uint64_t bookSyncFailed{}; // feed returned UNKNOWN (book kept as-is)
    std::uint64_t strategyCloses{}; // CloseIntents handed to the close sink
    std::uint64_t closeDropped{};   // strategy closes without a broker dealId
};

class StrategyRunner {
public:
    // `positions` may be empty when no spec caps open trades (unit tests, a
    // deployment without the position producer); a spec that DOES cap them
    // then fails closed — every entry blocks, warned once at start().
    // `feed` + `closeSink` come as a pair (warned at start() if only one is
    // wired): the feed mirrors broker deals into each worker's book, the
    // close sink carries strategy-initiated closes back to the broker.
    // bookSyncInterval throttles the feed per worker (checked per tick).
    StrategyRunner(std::vector<WorkerSpec> specs, TradeGate gate, OrderSink sink,
                   PositionCounter positions = {}, PositionFeed feed = {},
                   CloseSink closeSink = {},
                   std::chrono::seconds bookSyncInterval =
                       std::chrono::seconds{15});
    ~StrategyRunner();  // stops and joins if still running

    StrategyRunner(const StrategyRunner&) = delete;
    StrategyRunner& operator=(const StrategyRunner&) = delete;

    // Spawns one plain std::thread per spec. Plain thread + condition_variable
    // on purpose — std::jthread's stop_token wait (condition_variable_any)
    // doesn't link under `import std` here (see module-migration notes).
    void start();

    // UdpReceiver thread: route the tick to every worker cached for its
    // symbol. Enqueue + notify only — never blocks on strategy work.
    void onTick(const PriceData& tick);

    // Workers drain their queues, then exit and are joined — shutdown is
    // lossless and deterministic under test. Safe to call twice.
    void stop();

    [[nodiscard]] RunnerStats stats() const;

private:
    struct Worker {
        explicit Worker(WorkerSpec s) : spec(std::move(s)) {
            // Register every declared timeframe before the first tick. A
            // malformed spec throws here — at runner construction, loudly —
            // not on a worker thread mid-stream.
            for (const bars::SeriesSpec& series : spec.barSeries) {
                barStore.registerSeries(series.minutes, series.count);
            }
            for (const rangebar::RangeBarSpec& series : spec.rangeSeries) {
                barStore.registerRangeSeries(series);
            }
            if (spec.gateSeries) {
                barStore.registerSeries(spec.gateSeries->minutes,
                                        spec.gateSeries->count);
            }
        }
        WorkerSpec spec;
        // The worker's shared bar pipeline, updated once per tick at the top
        // of processTick, before the gates and decide() — and its first
        // update prepopulates from QuestDB (see barStore). Worker-thread-only
        // state like recentOpens: no locking needed.
        bars::BarStore barStore;
        // The worker's mirror of the BROKER book: syncBook seeds/removes
        // trades from the PL#/PO# feed so a strategy's during() sees (and
        // may close) real positions. Empty when no feed is wired. The
        // broker still owns exits — no markToMarket/reviewStopAndLimit runs
        // here; closes flow out through the close diff, never in.
        TradeManager tradeManager;
        // Broker identity for the booked trade(s), keyed by symbol like the
        // TradeManager itself (at most one entry — one-symbol specs). The
        // side map is the authority for dealId/dealReference/broker size:
        // Trade cannot carry them (no dealId field, no setters).
        std::unordered_map<std::string, BookedPosition> sideMap;
        // Next steady-clock instant the feed may be consulted; the epoch
        // default makes the first tick sync immediately.
        std::chrono::steady_clock::time_point nextSyncAt{};
        // workerException trace throttle: a persistently-throwing strategy
        // would otherwise emit one trace document per tick. At most one doc
        // per 30s per worker, carrying the count it stands for.
        // Worker-thread-only state: no locking needed.
        std::chrono::steady_clock::time_point nextExceptionTraceAt{};
        std::uint64_t exceptionsSinceTrace{0};
        // Timestamps of orders emitted inside the sliding one-minute window,
        // in TICK time (same clock the backtest cap uses; live tick time is
        // wall clock anyway). Only pushed to while the cap is active, so it
        // holds at most maxTradesPerMinute entries. Worker-thread-only state:
        // no locking needed.
        std::deque<std::chrono::system_clock::time_point> recentOpens;
        std::mutex m;
        std::condition_variable cv;
        std::deque<PriceData> queue;
        std::thread thread;
    };

    void workerLoop(Worker& worker);
    void processTick(Worker& worker, const PriceData& tick);
    static void traceSignalBlocked(const Worker& worker,
                                   std::string_view reason,
                                   std::string_view direction);
    static void traceWorkerException(Worker& worker, std::string_view detail);
    bool belowTradeRateCap(Worker& worker,
                           std::chrono::system_clock::time_point now);
    bool belowOpenTradeCap(const Worker& worker);
    void syncBookIfDue(Worker& worker, const PriceData& tick);
    void emitClose(Worker& worker, const Trade& closed, const PriceData& tick);

    // Bounds a worker's backlog through a stall. Drop-OLDEST: for trading the
    // newest price is the one that matters, and a bounded queue keeps memory
    // flat if a strategy wedges (e.g. the gate's Redis timeout per signal).
    static constexpr std::size_t kMaxQueuedTicks = 8192;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::unordered_map<std::string, std::vector<Worker*>> bySymbol_;
    TradeGate gate_;
    OrderSink sink_;
    PositionCounter positions_;
    PositionFeed feed_;
    CloseSink closeSink_;
    std::chrono::seconds bookSyncInterval_;
    std::atomic<bool> stopping_{false};
    bool started_{false};

    std::atomic<std::uint64_t> routed_{0};
    std::atomic<std::uint64_t> ignoredSymbol_{0};
    std::atomic<std::uint64_t> queueDropped_{0};
    std::atomic<std::uint64_t> sessionSkipped_{0};
    std::atomic<std::uint64_t> conditionsSkipped_{0};
    std::atomic<std::uint64_t> signals_{0};
    std::atomic<std::uint64_t> rateBlocked_{0};
    std::atomic<std::uint64_t> positionBlocked_{0};
    std::atomic<std::uint64_t> lockBlocked_{0};
    std::atomic<std::uint64_t> ordersLogged_{0};
    std::atomic<std::uint64_t> bookSeeded_{0};
    std::atomic<std::uint64_t> bookRemoved_{0};
    std::atomic<std::uint64_t> bookSyncFailed_{0};
    std::atomic<std::uint64_t> strategyCloses_{0};
    std::atomic<std::uint64_t> closeDropped_{0};
};

}  // namespace live

namespace live {

namespace {

std::string_view directionName(const Direction direction) {
    return direction == Direction::LONG ? "LONG" : "SHORT";
}

}  // namespace

StrategyRunner::StrategyRunner(std::vector<WorkerSpec> specs, TradeGate gate,
                               OrderSink sink, PositionCounter positions,
                               PositionFeed feed, CloseSink closeSink,
                               const std::chrono::seconds bookSyncInterval)
    : gate_(std::move(gate)), sink_(std::move(sink)),
      positions_(std::move(positions)), feed_(std::move(feed)),
      closeSink_(std::move(closeSink)), bookSyncInterval_(bookSyncInterval) {
    workers_.reserve(specs.size());
    for (WorkerSpec& spec : specs) {
        workers_.push_back(std::make_unique<Worker>(std::move(spec)));
        Worker* worker = workers_.back().get();
        bySymbol_[worker->spec.symbol].push_back(worker);
    }
}

StrategyRunner::~StrategyRunner() { stop(); }

void StrategyRunner::start() {
    if (started_) {
        return;
    }
    // A previous stop() leaves the flag raised; without this reset a
    // stop()-then-start() sequence would spawn workers that drain whatever is
    // queued and exit immediately — a runner that still accepts ticks but
    // silently never processes them.
    stopping_.store(false, std::memory_order_seq_cst);
    started_ = true;
    // Fail-closed is silent per tick, so a missing counter next to an active
    // open-trades cap — a wiring bug or a producer not deployed — must be
    // loud here, or the runner looks alive while blocking every entry.
    if (!positions_) {
        for (const auto& worker : workers_) {
            if (worker->spec.maxOpenTrades > 0) {
                backtest_log::error(
                    "StrategyRunner: no position counter wired but "
                    + worker->spec.strategyName + " ("
                    + worker->spec.strategyUuid + ") caps open trades — its "
                    "entries will all fail closed");
            }
        }
    }
    // The feed and the close sink only make sense as a pair: a fed book
    // whose strategy closes go nowhere silently drops them, and a close
    // sink over an empty book can never fire. A wiring bug, not fatal —
    // loud once here.
    if (static_cast<bool>(feed_) != static_cast<bool>(closeSink_)) {
        backtest_log::error(
            feed_ ? "StrategyRunner: position feed wired without a close "
                    "sink — strategy closes will be dropped (and counted)"
                  : "StrategyRunner: close sink wired without a position "
                    "feed — the book stays empty and no close can fire");
    }
    for (auto& worker : workers_) {
        worker->thread = std::thread([this, w = worker.get()] { workerLoop(*w); });
    }
}

void StrategyRunner::onTick(const PriceData& tick) {
    const auto it = bySymbol_.find(tick.symbol);
    if (it == bySymbol_.end()) {
        ignoredSymbol_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (Worker* worker : it->second) {
        {
            const std::lock_guard<std::mutex> lock{worker->m};
            worker->queue.push_back(tick);
            if (worker->queue.size() > kMaxQueuedTicks) {
                worker->queue.pop_front();
                queueDropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        worker->cv.notify_one();
        routed_.fetch_add(1, std::memory_order_relaxed);
    }
}

void StrategyRunner::stop() {
    if (!started_) {
        return;
    }
    stopping_.store(true, std::memory_order_seq_cst);
    for (auto& worker : workers_) {
        // Touch the mutex between flag and notify so a worker mid-predicate
        // cannot miss the wakeup (the classic lost-notify race).
        { const std::lock_guard<std::mutex> lock{worker->m}; }
        worker->cv.notify_one();
    }
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    started_ = false;
}

void StrategyRunner::workerLoop(Worker& worker) {
    for (;;) {
        PriceData tick;
        {
            std::unique_lock<std::mutex> lock{worker.m};
            worker.cv.wait(lock, [&] {
                return stopping_.load(std::memory_order_relaxed) ||
                       !worker.queue.empty();
            });
            if (worker.queue.empty()) {
                return;  // stopping and drained
            }
            tick = std::move(worker.queue.front());
            worker.queue.pop_front();
        }
        // A throwing strategy (or gate/sink) must not take the live process
        // down — log and move on to the next tick. catch (...) as well: a
        // non-std exception escaping the thread function would std::terminate
        // the whole process.
        try {
            processTick(worker, tick);
        } catch (const std::exception& e) {
            // Suppress the live-logs ship: a wedged strategy throws on EVERY
            // tick, and shipping one document per tick is the exact storm the
            // workerException trace's 30s throttle exists to prevent — the
            // (throttled) trace carries the text off-box; stderr keeps the
            // per-tick line.
            const backtest_log::SinkSuppression suppression;
            backtest_log::error("StrategyRunner: " + worker.spec.strategyName
                                + " (" + worker.spec.strategyUuid
                                + ") threw on tick: " + e.what());
            traceWorkerException(worker, e.what());
        } catch (...) {
            const backtest_log::SinkSuppression suppression;
            backtest_log::error("StrategyRunner: " + worker.spec.strategyName
                                + " (" + worker.spec.strategyUuid
                                + ") threw a non-std exception on tick");
            traceWorkerException(worker, "non-std exception");
        }
    }
}

void StrategyRunner::processTick(Worker& worker, const PriceData& tick) {
    // Book first, so broker-side removals happen BEFORE the close-diff
    // snapshot below — a deal the broker closed must never read as a
    // strategy close — and fresh seeds are visible to this tick's
    // decide/during.
    syncBookIfDue(worker, tick);

    // Shared bar pipeline, EVERY tick, ungated (a gap would corrupt the ATR
    // and the strategies' histories) and BEFORE the gates: the ATR
    // conditions and decide() judge this tick against bar state that
    // already includes it — same ordering as the backtest loop, so the two
    // paths cannot diverge. The first update prepopulates from QuestDB
    // (see barStore), so a warm feed can trade from its very first tick.
    worker.barStore.update(tick);

    // Peak-hours entry gate, before decide(): out-of-session ticks open
    // nothing (skipped, never deferred — same doctrine as the caps). The
    // book sync above and the during()/close-diff below are NEVER gated.
    std::optional<conditions::Distances> distances;
    if (worker.spec.peakHoursOnly &&
        !market_hours::tradePermitted(tick.symbol, tick.timestamp)) {
        sessionSkipped_.fetch_add(1, std::memory_order_relaxed);
    } else if (worker.spec.gateSeries.has_value() &&
               !(distances = conditions::check(
                     worker.barStore, *worker.spec.gateSeries, tick,
                     worker.spec.vars.STOP_DISTANCE_IN_ATR,
                     worker.spec.vars.LIMIT_DISTANCE_IN_ATR))) {
        // ATR entry conditions failed: gate series not warm, spread too wide
        // vs ATR, or volatility below the pip floors — skip decide() like the
        // session gate above. Same conditions::check the backtest loop runs,
        // so backtest and live can never diverge here.
        conditionsSkipped_.fetch_add(1, std::memory_order_relaxed);
    } else if (const auto signal =
                   worker.spec.strategy->decide(tick, worker.barStore)) {
        signals_.fetch_add(1, std::memory_order_relaxed);
        const std::string direction{directionName(*signal)};
        // Cheapest check first: the rate window is in-memory, the position
        // count and the trade lock are Redis round trips. A blocked signal is
        // skipped, never deferred — same as the backtest caps.
        if (!belowTradeRateCap(worker, tick.timestamp)) {
            rateBlocked_.fetch_add(1, std::memory_order_relaxed);
            traceSignalBlocked(worker, "rateCap", direction);
        } else if (!belowOpenTradeCap(worker)) {
            positionBlocked_.fetch_add(1, std::memory_order_relaxed);
            traceSignalBlocked(worker, "openTradeCap", direction);
        } else if (gate_(worker.spec.strategyUuid, direction)) {
            sink_(OrderIntent{
                .strategyName = worker.spec.strategyName,
                .strategyUuid = worker.spec.strategyUuid,
                .symbol = worker.spec.symbol,
                .direction = *signal,
                .size = worker.spec.vars.TRADING_SIZE,
                // Dynamic ATR-derived pip distances when the gate is
                // engaged; the raw variables (test-only path) otherwise.
                .stopDistancePips = distances
                                        ? distances->stopPips
                                        : worker.spec.vars.STOP_DISTANCE_IN_ATR,
                .limitDistancePips =
                    distances ? distances->limitPips
                              : worker.spec.vars.LIMIT_DISTANCE_IN_ATR,
                .bid = tick.bid,
                .ask = tick.ask,
                .timestamp = tick.timestamp,
            });
            if (worker.spec.maxTradesPerMinute > 0) {
                worker.recentOpens.push_back(tick.timestamp);
            }
            ordersLogged_.fetch_add(1, std::memory_order_relaxed);
        } else {
            lockBlocked_.fetch_add(1, std::memory_order_relaxed);
            traceSignalBlocked(worker, "tradeLock", direction);
        }
    }
    // TradeManager has no close callback, so a strategy close inside
    // during() is detected by diffing the closed-trades log around the call
    // (closedTrades is append-only; syncBook's own removals happened above).
    const std::size_t closedBefore =
        worker.tradeManager.getClosedTrades().size();
    worker.spec.strategy->during(tick, worker.barStore, worker.tradeManager);
    const auto& closedTrades = worker.tradeManager.getClosedTrades();
    for (std::size_t i = closedBefore; i < closedTrades.size(); ++i) {
        emitClose(worker, closedTrades[i], tick);
    }
}

void StrategyRunner::syncBookIfDue(Worker& worker, const PriceData& tick) {
    if (!feed_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < worker.nextSyncAt) {
        return;
    }
    worker.nextSyncAt = now + bookSyncInterval_;

    const auto fetched = feed_(worker.spec.strategyUuid, worker.spec.symbol);
    if (!fetched) {
        // FAIL-OPEN for book state (see PositionFeed): keep the last known
        // book rather than tearing it down on a Redis blip.
        bookSyncFailed_.fetch_add(1, std::memory_order_relaxed);
        if (live_trace::enabled()) {
            live_trace::emit("bookSyncFailed",
                             {.strategyUuid = worker.spec.strategyUuid,
                              .strategyName = worker.spec.strategyName,
                              .symbol = worker.spec.symbol});
        }
        return;
    }

    const std::string& symbol = worker.spec.symbol;
    if (const auto booked = worker.sideMap.find(symbol);
        booked != worker.sideMap.end()) {
        const auto match = std::ranges::find_if(
            *fetched, [&](const BookedPosition& position) {
                return position.dealReference == booked->second.dealReference;
            });
        if (match != fetched->end()) {
            // Still open at the broker. Refresh the broker identity — this
            // is how a confirm-resolved (or producer-reconciled) dealId
            // reaches the side map after the open booked without one.
            booked->second.dealId = match->dealId;
            booked->second.brokerSize = match->brokerSize;
        } else {
            // Gone from the feed: closed at the broker, pruned by the
            // producer, or its PO# expired unconfirmed — either way it no
            // longer exists as far as anyone can tell. Remove from the book
            // WITHOUT a CloseIntent (nothing to close). closeTrade logs one
            // "Trade Closed" line; its book-keeping PnL against the last
            // mark is cosmetic — the broker owns the real PnL.
            const Trade* trade = worker.tradeManager.findActiveTrade(symbol);
            const std::int32_t closePrice =
                trade != nullptr ? trade->lastMarkPrice : tick.bid;
            // Copied, not referenced: the erase below invalidates `booked`
            // and the trace still needs the broker identity.
            const BookedPosition removed = booked->second;
            worker.tradeManager.closeTrade(symbol, closePrice, tick);
            worker.sideMap.erase(symbol);
            bookRemoved_.fetch_add(1, std::memory_order_relaxed);
            if (live_trace::enabled()) {
                live_trace::emit(
                    "bookRemoved",
                    {.strategyUuid = worker.spec.strategyUuid,
                     .strategyName = worker.spec.strategyName,
                     .symbol = symbol,
                     .dealReference = removed.dealReference,
                     .dealId = removed.dealId},
                    {{"direction", directionName(removed.direction)},
                     {"closePrice", static_cast<std::int64_t>(closePrice)}});
            }
        }
    }

    if (!worker.sideMap.contains(symbol) && !fetched->empty() &&
        !worker.tradeManager.hasActiveTradeForSymbol(symbol)) {
        // Seed the OLDEST deal (PL# append order): the book holds at most
        // one trade per symbol by TradeManager design, so surplus deals
        // (maxOpenTrades > 1, or LONG+SHORT after a lock lapse) are
        // acknowledged in the log but not booked — the close path only ever
        // addresses the booked one.
        const BookedPosition& first = fetched->front();
        const PriceData synthetic{first.level, first.level, first.openedAt,
                                  symbol};
        worker.tradeManager.openTrade(synthetic, first.engineSize,
                                      first.direction, 0, 0);
        worker.sideMap.emplace(symbol, first);
        bookSeeded_.fetch_add(1, std::memory_order_relaxed);
        if (live_trace::enabled()) {
            live_trace::emit(
                "bookSeeded",
                {.strategyUuid = worker.spec.strategyUuid,
                 .strategyName = worker.spec.strategyName,
                 .symbol = symbol,
                 .dealReference = first.dealReference,
                 .dealId = first.dealId},
                {{"direction", directionName(first.direction)},
                 {"brokerSize", first.brokerSize},
                 {"engineSize", static_cast<std::int64_t>(first.engineSize)},
                 {"level", static_cast<std::int64_t>(first.level)},
                 {"brokerDeals", fetched->size()}});
        }
        if (fetched->size() > 1) {
            backtest_log::error(
                "StrategyRunner: " + worker.spec.strategyName + " ("
                + worker.spec.strategyUuid + ") has "
                + std::to_string(fetched->size()) + " broker deals for "
                + symbol + " — booking only the oldest ("
                + first.dealReference + ")");
        }
    }
}

void StrategyRunner::emitClose(Worker& worker, const Trade& closed,
                               const PriceData& tick) {
    const auto booked = worker.sideMap.find(closed.symbol);
    if (!closeSink_ || booked == worker.sideMap.end() ||
        booked->second.dealId.empty()) {
        // No broker identity to address — the deal never confirmed (or the
        // book drifted). Count + log; the deal is still in Redis, so the
        // next sync re-seeds the book and the strategy may try again once
        // an id has appeared.
        closeDropped_.fetch_add(1, std::memory_order_relaxed);
        backtest_log::error(
            "StrategyRunner: dropping strategy close of " + closed.symbol
            + " for " + worker.spec.strategyName + " ("
            + worker.spec.strategyUuid + ") — "
            + (closeSink_ ? "no confirmed broker dealId"
                          : "no close sink wired (warned at start)"));
        if (live_trace::enabled()) {
            live_trace::emit(
                "closeDropped",
                {.strategyUuid = worker.spec.strategyUuid,
                 .strategyName = worker.spec.strategyName,
                 .symbol = closed.symbol,
                 .dealReference =
                     booked != worker.sideMap.end()
                         ? std::string_view{booked->second.dealReference}
                         : std::string_view{}},
                {{"reason", closeSink_ ? "noDealId" : "noCloseSink"}});
        }
        if (booked != worker.sideMap.end()) {
            worker.sideMap.erase(booked);
        }
        return;
    }
    closeSink_(CloseIntent{
        .strategyName = worker.spec.strategyName,
        .strategyUuid = worker.spec.strategyUuid,
        .symbol = closed.symbol,
        .direction = booked->second.direction,
        .brokerSize = booked->second.brokerSize,
        .dealId = booked->second.dealId,
        .dealReference = booked->second.dealReference,
        .timestamp = tick.timestamp,
    });
    strategyCloses_.fetch_add(1, std::memory_order_relaxed);
    // Erase either way: if the broker close succeeded the deal leaves Redis
    // too; if it failed, the deal is still there and the next sync re-seeds
    // book + side map — a natural, sync-interval-throttled retry.
    worker.sideMap.erase(booked);
}

void StrategyRunner::traceSignalBlocked(const Worker& worker,
                                        const std::string_view reason,
                                        const std::string_view direction) {
    if (!live_trace::enabled()) {
        return;
    }
    live_trace::emit("signalBlocked",
                     {.strategyUuid = worker.spec.strategyUuid,
                      .strategyName = worker.spec.strategyName,
                      .symbol = worker.spec.symbol},
                     {{"reason", reason}, {"direction", direction}});
}

void StrategyRunner::traceWorkerException(Worker& worker,
                                          const std::string_view detail) {
    if (!live_trace::enabled()) {
        return;
    }
    ++worker.exceptionsSinceTrace;
    const auto now = std::chrono::steady_clock::now();
    if (now < worker.nextExceptionTraceAt) {
        return;  // counted; the next emitted doc carries the total
    }
    worker.nextExceptionTraceAt = now + std::chrono::seconds{30};
    live_trace::emit("workerException",
                     {.strategyUuid = worker.spec.strategyUuid,
                      .strategyName = worker.spec.strategyName,
                      .symbol = worker.spec.symbol},
                     {{"detail", detail},
                      {"occurrences", worker.exceptionsSinceTrace}});
    worker.exceptionsSinceTrace = 0;
}

bool StrategyRunner::belowTradeRateCap(
    Worker& worker, const std::chrono::system_clock::time_point now) {
    if (worker.spec.maxTradesPerMinute <= 0) {
        return true;
    }
    // Same half-open window as the backtest cap (runLoop.cppm): an order
    // exactly 60 seconds old has aged out and frees its slot on this tick.
    // Broker ticks arrive wall-clock-ordered; one arriving with an older
    // timestamp just evicts nothing, which can only over-block, never let an
    // extra order through.
    while (!worker.recentOpens.empty() &&
           now - worker.recentOpens.front() >= std::chrono::minutes{1}) {
        worker.recentOpens.pop_front();
    }
    return worker.recentOpens.size() <
           static_cast<std::size_t>(worker.spec.maxTradesPerMinute);
}

bool StrategyRunner::belowOpenTradeCap(const Worker& worker) {
    if (worker.spec.maxOpenTrades <= 0) {
        return true;
    }
    // FAIL-CLOSED, same doctrine as the trade lock: no counter wired (warned
    // at start()) or an unknown count (Redis unreachable — already logged
    // down in the client) blocks the entry, because a missed entry is
    // recoverable and a position over the cap is not.
    if (!positions_) {
        return false;
    }
    const std::optional<int> open = positions_(worker.spec.strategyUuid);
    return open && *open < worker.spec.maxOpenTrades;
}

RunnerStats StrategyRunner::stats() const {
    return RunnerStats{
        .routed = routed_.load(std::memory_order_relaxed),
        .ignoredSymbol = ignoredSymbol_.load(std::memory_order_relaxed),
        .queueDropped = queueDropped_.load(std::memory_order_relaxed),
        .sessionSkipped = sessionSkipped_.load(std::memory_order_relaxed),
        .conditionsSkipped = conditionsSkipped_.load(std::memory_order_relaxed),
        .signals = signals_.load(std::memory_order_relaxed),
        .rateBlocked = rateBlocked_.load(std::memory_order_relaxed),
        .positionBlocked = positionBlocked_.load(std::memory_order_relaxed),
        .lockBlocked = lockBlocked_.load(std::memory_order_relaxed),
        .ordersLogged = ordersLogged_.load(std::memory_order_relaxed),
        .bookSeeded = bookSeeded_.load(std::memory_order_relaxed),
        .bookRemoved = bookRemoved_.load(std::memory_order_relaxed),
        .bookSyncFailed = bookSyncFailed_.load(std::memory_order_relaxed),
        .strategyCloses = strategyCloses_.load(std::memory_order_relaxed),
        .closeDropped = closeDropped_.load(std::memory_order_relaxed),
    };
}

}  // namespace live
