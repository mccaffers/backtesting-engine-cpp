# Quickstart

How to build the engine and use each subcommand. Install the toolchain and system libraries first — see [REQUIREMENTS.md](REQUIREMENTS.md). Runtime configuration is environment-driven — see [ENVIRONMENT.md](ENVIRONMENT.md). For how the pieces fit together, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Build

```bash
git clone --recurse-submodules https://github.com/mccaffers/backtesting-engine-cpp
cd backtesting-engine-cpp

# One-off: vendored dependency builds (libpqxx, AWS SDK)
bash ./scripts/build_dep.sh

# Configure (CMake + Ninja + Clang/libc++) and compile
bash ./scripts/build.sh
```

`build.sh` runs `build_dep.sh` automatically on first use, picks the right toolchain per platform (Homebrew LLVM on macOS, `$CC`/`$CXX` or `clang` elsewhere), and produces `build/BacktestingEngine`.

The binary dispatches on its first argument. The subcommands are:

```
BacktestingEngine <load|run|experiments|analysis|ingest|live|tracking|positions> [args...]
```

(There is no `--help` — a missing or unknown subcommand just prints an error and exits 1.)

## `load` — queue a parameter sweep

Expands a strategy parameter sweep into individual backtest payloads and pushes them onto the Redis work queue. Needs **Redis** running.

```
BacktestingEngine load <random|ohlcBreakout|fvg|keltnerFade|sessionRangeBreakout|squeezeBreakout|nyOpenRangeBreakout|liquiditySweepReversal|rangeVelocity>
```

The sweep name is required (exact, case-sensitive): `random` (random entries, SL/TP grid — a baseline harness), `ohlcBreakout` (multi-bar range breakout, EMA trend filter), `fvg` (fair-value-gap retracement), `keltnerFade` (volatility-band mean-reversion fade), `sessionRangeBreakout` (London open-range breakout), `squeezeBreakout` (compressed-bar breakout), `nyOpenRangeBreakout` (NY open-range breakout), `liquiditySweepReversal` (swing-pivot sweep + displacement reversal), or `rangeVelocity` (range-bar run velocity breakout). An unknown or missing name prints the valid set and exits 1. Before touching Redis it prints the total sweep size (combinations × symbol groups) and waits for confirmation on stdin — closed stdin counts as a decline, so a non-interactive invocation can't accidentally queue an enormous grid. (`keltnerFade` was retired from the default batch in 2026-28 — its 7 winners all landed on the EURUSD control symbol — but the sweep module remains; load it explicitly to re-test.)

To queue the sweep as a slippage stress run, export `ENTRY_SLIPPAGE_TENTH_PIPS` (e.g. `3` = every entry fills 0.3 pip against the trade) before `load` — the value is frozen into every queued run and carried through the rolling-window ladder (see [ENVIRONMENT.md](ENVIRONMENT.md)).

```bash
# via the wrapper script (builds first, checks Redis reachability)
REDIS_HOST=127.0.0.1 bash scripts/load.sh random

# or directly
./build/BacktestingEngine load random
```

Invoked with no argument, `scripts/load.sh` queues a seven-strategy batch — `ohlcBreakout fvg sessionRangeBreakout squeezeBreakout nyOpenRangeBreakout liquiditySweepReversal rangeVelocity` — deliberately excluding `random` and `keltnerFade`, and exports a single `BACKTEST_BATCH` label first so every strategy lands in the same weekly indices.

Each symbol group becomes its own run: payload keys are written first, then the payload key-names, then the run descriptor is advertised on `BACKTESTING_QUEUE_RUN` — so a worker never sees a run whose strategies aren't fully present. (Key details in [ARCHITECTURE.md](ARCHITECTURE.md#backtest-pipeline).)

## `run` — execute backtests

Drains the Redis queue and runs backtests against QuestDB tick data, reporting results to Elasticsearch. Needs **Redis**, **QuestDB**, and (unless `ELASTIC_ENABLED=0`) **Elasticsearch**.

```
BacktestingEngine run <questdb-host>                  # queue mode: drain the Redis queue
BacktestingEngine run <questdb-host> <base64-config>  # direct mode: run one decoded strategy, bypassing Redis
```

```bash
# via the wrapper script (validates env vars, builds, checks Redis, runs queue mode)
infisical run -- bash ./scripts/run.sh
# or without Infisical:
ELASTIC_HOST=http://localhost:9200 ELASTIC_USER=elastic ELASTIC_USER_PASSWORD=password \
  REDIS_HOST=localhost bash scripts/run.sh

# or directly
./build/BacktestingEngine run localhost

```

Wrapper caveats: `run.sh` sources `scripts/clean.sh` first (pass `--clean` to wipe `build/` for a from-scratch rebuild), hardcodes `run localhost` — a remote QuestDB host can't be passed through the wrapper — and if Redis is unreachable it prints "skipping" and exits 0.

Queue mode runs as a daemon — it never exits on an empty queue, it logs `run queues empty, waiting for work...` and re-peeks on a 1s timer until work reappears (like `analysis`); multiple `run` processes (across machines) can share one Redis and drain the same run in parallel. At startup it also maps a shared-memory control channel (`ipc::EngineControlChannel`, a 12-byte block at `/tmp/EngineControlShm` — `source/shared/ipc/engineControl.hpp`) that broadcasts the in-flight backtest count and reads a stop flag a local monitor can set: flipping the stop signal is the only graceful way to stop a worker — it finishes in-flight backtests, claims nothing more, and parks drained-and-paused. Best-effort: if the segment can't map, the engine still runs, just unmonitored. Tick data is cached across runs — one QuestDB superset load per symbol set serves every rolling window as a slice, and backtests from consecutive runs pipeline on the thread pool (tunable via the `TICK_CACHE_*` variables in [ENVIRONMENT.md](ENVIRONMENT.md)). Completed runs automatically re-queue themselves on the next rolling window — see the ladder in [ARCHITECTURE.md](ARCHITECTURE.md#rolling-window-chaining). A helper for producing a base64 config for direct mode lives in `scripts/arguments/build.sh`; direct mode bypasses both Redis and the tick cache.

## `experiments` — queue an occurrence-rate experiment sweep

Expands a parameter sweep of chained-activity experiments ("price drops X% in W minutes, then recovers Y% in Z minutes — how often?") into payloads on the experiment Redis queue — `load`'s twin for questions that don't need a strategy. Needs **Redis** running (and Elasticsearch for the weekly index/alias admin unless `ELASTIC_ENABLED=0`).

```
BacktestingEngine experiments <dipRecovery>
```

The sweep name is required: `dipRecovery` (drop size × drop window × recovery size × recovery window — one experiment per combination, evaluated per symbol group). An unknown or missing name prints the valid set and exits 1. Like `load`, it prints the total sweep size and waits for confirmation on stdin before touching Redis, and each symbol group becomes its own run on `BACKTESTING_QUEUE_EXPERIMENT_RUN`. The tick-history window (LAST_MONTHS/OFFSET_MONTHS, default 9/0) is declared per sweep, next to its grid.

```bash
./build/BacktestingEngine experiments dipRecovery
```

## `analysis` — evaluate queued experiments

Drains the experiment queue and counts each experiment's occurrences against QuestDB tick data, reporting one aggregate document per experiment × symbol group to the weekly `backtesting-experiments` Elasticsearch index — `run`'s twin for the experiment queue. Needs **Redis**, **QuestDB**, and (unless `ELASTIC_ENABLED=0`) **Elasticsearch**. Runs as a daemon (Ctrl+C to stop — no shared-memory stop channel).

```
BacktestingEngine analysis <questdb-host>
```

```bash
./build/BacktestingEngine analysis localhost
```

Each document echoes the experiment chain and carries occurrence counts plus the strategy-shaping stats: `completionRate` (attempts vs completions), per-leg failure attribution, month/hour occurrence buckets, per-symbol coverage, MFE/MAE excursion quantiles (completed and failed attempts separately), completion-time quantiles, and mean spread at trigger. Multiple `analysis` workers can share the queue exactly like `run` workers. See [ARCHITECTURE.md](ARCHITECTURE.md#experiments).

## `ingest` — stream ticks into QuestDB

Binds a UDP socket, decodes incoming tick packets, and batch-writes them to QuestDB via ILP-over-HTTP. Needs **QuestDB** (it still binds and buffers if QuestDB is down, shedding once the buffer fills). Blocks until SIGINT/SIGTERM.

```
BacktestingEngine ingest [udp-port]    # port defaults to $INGEST_UDP_PORT, then 11111
```

```bash
bash scripts/ingest.sh         # bind 11111
bash scripts/ingest.sh 22222   # bind 22222
```

The script probes QuestDB reachability and warns (but continues) if it's down.

## `live` — live trading

Pulls the winning backtest runs from the `backtesting-winners-current` Elasticsearch alias (the newest weekly winners index, repointed by each `load`), instantiates their strategies (one worker thread per symbol), and routes a live UDP tick stream to them. Entries are gated by Redis trade locks, position caps, portfolio cluster-exposure limits, and (for winners flagged `PEAK_HOURS_ONLY`) peak market sessions; orders are placed with the **IG REST API**, with session credentials pulled from DynamoDB. The broker owns positions and exits — the engine mirrors the position book *from* Redis and only emits opens/closes. Blocks until SIGINT/SIGTERM.

Needs **Elasticsearch** (the `backtesting-winners-current` alias must exist — run one `load` first, or hand-park the alias on the legacy `trading_results` index — with at least one full-history run above `LIVE_MIN_SCORE` and `LIVE_MIN_CALMAR_SCORE`, within `LIVE_MAX_DRAWDOWN_PERCENT`; the shorter rolling-window rungs don't qualify — no winners is the one hard startup failure, exit 1), **Redis**, **QuestDB** (with `OHLC_PREPOPULATE=1`, the default, OHLC and range-bar histories seed from it at each symbol's first tick; a failed seed logs and falls back to a cold start), and a live tick feed on UDP. **AWS credentials** (DynamoDB) are effectively required to trade but not to start: without an IG session the engine warns and continues — every order fails and extends its trade lock, so it runs as an effective dry run.

```
BacktestingEngine live [udp-port]    # port defaults to $LIVE_UDP_PORT, then 11110
```

```bash
bash scripts/live.sh           # bind 11110
bash scripts/live.sh 22222     # bind 22222
```

Set `TRADING_ENVIRONMENT=demo|live` to select the IG environment (credentials key `Auth#<env>` in DynamoDB). A `LiveReporter` prints per-minute stats and a final summary on shutdown.

## `tracking` — follow the account's deal updates

Binds a UDP socket for the IG streamer's deal feed (256-byte deal packets — position opens, updates, and deletions pushed by IG's Lightstreamer `TRADE:*` channel) and prints one timestamped log line per decoded deal. On a DELETED (closed) deal it also computes the close pips and archives the position's Redis book entry (`PO#` → `PH#`, pruning the `PL#` list), and every deal ships a `live-trades` document to Elasticsearch. Uses **Redis** and **Elasticsearch**, but degrades gracefully without either — an unreachable Redis means fallback documents (never a crash), and undeliverable documents land in the dead-letter file. Blocks until SIGINT/SIGTERM.

```
BacktestingEngine tracking [udp-port]    # port defaults to $TRACKING_UDP_PORT, then 11112
```

Datagrams that aren't exactly 256 bytes are counted as dropped (totals are printed at shutdown). The wire format and decoder live in `source/tracking/dealPacket.cppm` — see [ARCHITECTURE.md](ARCHITECTURE.md#tracking).

## `positions` — mirror the IG position book into Redis

The in-process replacement for the external C# `igmarkets_positions` cron: every minute it GETs the account's open positions from the **IG REST API** and mirrors them into **Redis** — refreshing the `PO#` payloads and `PL#` strategy books that `live` reads, rebuilding the `CG#` cluster-exposure sets, and pruning deals the broker no longer reports. The first sync fires immediately, then one per minute; each cycle is reported to Elasticsearch (`live-function-logs`) unless `ELASTIC_ENABLED=0`. Loops until SIGINT/SIGTERM.

Needs **Redis**, **AWS credentials** (IG session tokens from DynamoDB), and the **IG REST API**.

```
BacktestingEngine positions           # loop forever, one sync per minute
BacktestingEngine positions --once    # single cycle, then exit (cron parity / smoke test)
```

Set `TRADING_ENVIRONMENT=demo|live` to select the IG environment (credentials key `Auth#<env>` in DynamoDB). Without a session every cycle logs a warning, files a FAILED report, and touches nothing — safe to leave running until the login service refreshes the tokens. See [ARCHITECTURE.md](ARCHITECTURE.md#positions).

## Tests

Catch2 tests run through ctest:

```bash
bash ./scripts/test.sh          # build + ctest --output-on-failure
CLEAN=1 bash ./scripts/test.sh  # force a clean reconfigure first
```

For a local coverage report there is `scripts/local_test_coverage.sh` (Clang source-based coverage, same instrumentation CI uses for SonarCloud).
