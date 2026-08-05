// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// liveReporter — the live subcommand's once-a-minute throughput report, on a
// background thread (receiver.run() blocks the main thread). Borrows the
// command's receive/drop counters and the runner by reference: the reporter
// must be stopped (or destroyed — the destructor stops it) before they go
// away, which the command guarantees by declaring it after them.

export module liveReporter;

import std;
import backtestLog;         // backtest_log::logLine
import liveStrategyRunner;  // live::StrategyRunner, RunnerStats
import liveTrace;           // live_trace::emit — stats/shutdown documents

export namespace live {

class LiveReporter {
public:
    LiveReporter(const std::atomic<std::uint64_t>& received,
                 const std::atomic<std::uint64_t>& decodeDropped,
                 const StrategyRunner& runner)
        : received_(received), decodeDropped_(decodeDropped), runner_(runner) {}

    ~LiveReporter() { stop(); }

    LiveReporter(const LiveReporter&) = delete;
    LiveReporter& operator=(const LiveReporter&) = delete;

    // Spawns the reporting thread. It wakes every second to notice stop()
    // promptly. Plain std::thread + atomic flag on purpose — std::jthread's
    // stop_token wait (condition_variable_any) doesn't link under
    // `import std` here (see module-migration notes).
    void start();

    // Joins the reporting thread. Safe to call twice.
    void stop();

    // The end-of-run summary line ("shutting down"); call after the runner
    // has stopped so the printed stats are final.
    void logFinalSummary() const;

private:
    void loop();

    const std::atomic<std::uint64_t>& received_;
    const std::atomic<std::uint64_t>& decodeDropped_;
    const StrategyRunner& runner_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace live

namespace live {

void LiveReporter::start() {
    if (running_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    thread_ = std::thread([this] { loop(); });
}

void LiveReporter::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LiveReporter::loop() {
    using clock = std::chrono::steady_clock;
    constexpr auto interval = std::chrono::minutes(1);
    std::uint64_t lastReceived = 0;
    auto nextReport = clock::now() + interval;
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (clock::now() < nextReport) {
            continue;
        }
        nextReport += interval;
        const auto total = received_.load(std::memory_order_relaxed);
        const auto delta = total - lastReceived;
        lastReceived = total;
        const RunnerStats stats = runner_.stats();
        backtest_log::logLine(
            "LiveCommand: received={} (+{}, ~{}/s); decode-dropped={}; "
            "routed={} ignoredSymbol={} queueDropped={} sessionSkipped={} "
            "conditionsSkipped={} signals={} rateBlocked={} "
            "positionBlocked={} lockBlocked={} orders={} bookSeeded={} "
            "bookRemoved={} bookSyncFailed={} strategyCloses={} "
            "closeDropped={}",
            total, delta, delta / 60,
            decodeDropped_.load(std::memory_order_relaxed), stats.routed,
            stats.ignoredSymbol, stats.queueDropped, stats.sessionSkipped,
            stats.conditionsSkipped, stats.signals, stats.rateBlocked,
            stats.positionBlocked, stats.lockBlocked, stats.ordersLogged,
            stats.bookSeeded, stats.bookRemoved, stats.bookSyncFailed,
            stats.strategyCloses, stats.closeDropped);
        if (live_trace::enabled()) {
            live_trace::emit(
                "stats", {},
                {{"received", total},
                 {"receivedDelta", delta},
                 {"decodeDropped",
                  decodeDropped_.load(std::memory_order_relaxed)},
                 {"routed", stats.routed},
                 {"ignoredSymbol", stats.ignoredSymbol},
                 {"queueDropped", stats.queueDropped},
                 {"sessionSkipped", stats.sessionSkipped},
                 {"conditionsSkipped", stats.conditionsSkipped},
                 {"signals", stats.signals},
                 {"rateBlocked", stats.rateBlocked},
                 {"positionBlocked", stats.positionBlocked},
                 {"lockBlocked", stats.lockBlocked},
                 {"orders", stats.ordersLogged},
                 {"bookSeeded", stats.bookSeeded},
                 {"bookRemoved", stats.bookRemoved},
                 {"bookSyncFailed", stats.bookSyncFailed},
                 {"strategyCloses", stats.strategyCloses},
                 {"closeDropped", stats.closeDropped}});
        }
    }
}

void LiveReporter::logFinalSummary() const {
    // decode-dropped: bad size / unknown symbol / corrupt fields.
    const RunnerStats stats = runner_.stats();
    backtest_log::logLine(
        "LiveCommand: shutting down (received={}, decode-dropped={}, "
        "routed={}, ignoredSymbol={}, queueDropped={}, sessionSkipped={}, "
        "conditionsSkipped={}, signals={}, rateBlocked={}, "
        "positionBlocked={}, lockBlocked={}, orders={}, bookSeeded={}, "
        "bookRemoved={}, bookSyncFailed={}, strategyCloses={}, "
        "closeDropped={})",
        received_.load(), decodeDropped_.load(), stats.routed,
        stats.ignoredSymbol, stats.queueDropped, stats.sessionSkipped,
        stats.conditionsSkipped, stats.signals, stats.rateBlocked,
        stats.positionBlocked, stats.lockBlocked, stats.ordersLogged,
        stats.bookSeeded, stats.bookRemoved, stats.bookSyncFailed,
        stats.strategyCloses, stats.closeDropped);
    if (live_trace::enabled()) {
        live_trace::emit(
            "shutdown", {},
            {{"received", received_.load()},
             {"decodeDropped", decodeDropped_.load()},
             {"routed", stats.routed},
             {"ignoredSymbol", stats.ignoredSymbol},
             {"queueDropped", stats.queueDropped},
             {"sessionSkipped", stats.sessionSkipped},
             {"conditionsSkipped", stats.conditionsSkipped},
             {"signals", stats.signals},
             {"rateBlocked", stats.rateBlocked},
             {"positionBlocked", stats.positionBlocked},
             {"lockBlocked", stats.lockBlocked},
             {"orders", stats.ordersLogged},
             {"bookSeeded", stats.bookSeeded},
             {"bookRemoved", stats.bookRemoved},
             {"bookSyncFailed", stats.bookSyncFailed},
             {"strategyCloses", stats.strategyCloses},
             {"closeDropped", stats.closeDropped}});
    }
}

}  // namespace live
