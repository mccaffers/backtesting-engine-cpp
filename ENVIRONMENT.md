# Environment Variables

All engine configuration is read from the environment via `env::getOr(name, fallback)` (`source/shared/utilities/env.cpp`) — every variable is **optional with a default** unless noted. An unset *or empty* variable falls back to its default.

The `run` and `analysis` subcommands print a startup diagnostic dump of the whole environment to stderr, masking any variable whose name contains `PASSWORD`, `PASSWD`, `SECRET`, `TOKEN`, `CREDENTIAL`, `KEY`, or `AUTH`.

## Quick reference

| Variable | Default | `load` | `experiments` | `run` | `analysis` | `ingest` | `live` | `tracking` | `positions` | Purpose |
| --- | --- | :-: | :-: | :-: | :-: | :-: | :-: | :-: | :-: | --- |
| `REDIS_HOST` | `127.0.0.1` | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | Redis host (port is always `6379`) |
| `BACKTEST_BATCH` | *(current UTC ISO week)* | ✓ | ✓ | | | | | | | Weekly batch label (`YYYY-WW`, e.g. `2026-28`) naming the Elasticsearch outcome indices (`backtesting-results-2026-28`, ...; `experiments`: `backtesting-experiments-2026-28`). Frozen into every queued run descriptor and carried through all rolling-window rungs; `scripts/load.sh` exports it once so its per-strategy invocations share one label. Set explicitly to re-run into a past week's indices |
| `ENTRY_SLIPPAGE_TENTH_PIPS` | `0` | ✓ | | | | | | | | Slippage stress toggle: every backtest entry fills this many **tenths of a pip** against the trade (`3` = 0.3 pip; SL/TP anchors stay on the raw tick). Read once at `load` and frozen into every queued run descriptor, carried through all rolling-window rungs, and recorded in each run's results doc. **Validated** — a non-integer or negative value fails the load |
| `QUESTDB_HOST` | `127.0.0.1` | | | ✓ | | ✓ | ✓ | | | QuestDB host (`run`: the required `<questdb-host>` argument covers tick reads only — the OHLC/range-bar warm-up still connects to `$QUESTDB_HOST`, so `run <remote-host>` reads ticks from the argv host but seeds bar histories from here; `live`: OHLC pre-population reads) |
| `QUESTDB_PORT` | `8812` | | | ✓ | ✓ | | ✓ | | | QuestDB pgwire port for tick reads (`live`: OHLC pre-population reads). **Validated** — a non-numeric value is a hard failure |
| `QUESTDB_ILP_PORT` | `9000` | | | | | ✓ | | | | QuestDB ILP-over-HTTP port for tick writes |
| `TICK_CACHE_TTL_MINUTES` | `60` | | | ✓ | | | | | | Lifetime of a cached tick superset before it is reloaded from QuestDB (queue mode only — direct mode and `analysis` bypass the cache). **Validated** — a non-numeric or negative value throws on the cache's first use (once the first run is claimed), aborting the worker; `0` passes |
| `TICK_CACHE_MAX_SUPERSETS` | `1` | | | ✓ | | | | | | How many tick supersets (one per symbol set) a worker keeps resident before evicting the oldest. **Validated** — same rule as the TTL |
| `ELASTIC_ENABLED` | `1` | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | Master toggle for Elasticsearch reporting; set `0` to disable (`load`/`experiments`: also skips the weekly index/alias admin; `live`: also silences the trade audit, the trace documents, and the `live-logs` log feed; `tracking`: also silences the per-deal `live-trades` documents; `positions`: also silences the per-cycle `live-function-logs` reports) |
| `ELASTIC_HOST` | `http://localhost:9200` | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | Elasticsearch base URL (results reporting; weekly index/alias admin at `load`/`experiments`; `analysis` experiment documents; live winner source + trade audit; `tracking` deal documents; `positions` cycle reports) |
| `ELASTIC_USER` | *(empty)* | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | Basic-auth username; empty means no auth header |
| `ELASTIC_USER_PASSWORD` | *(empty)* | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | Basic-auth password |
| `ELASTIC_TRADES_ENABLED` | `0` | | | ✓ | | | | | | Set `1` to bulk-index every closed trade into `backtesting-trades` |
| `ELASTIC_DEADLETTER_PATH` | `elastic_deadletter.ndjson` | | | ✓ | ✓ | | ✓ | ✓ | ✓ | File that failed Elasticsearch documents are appended to after retries are exhausted |
| `ELASTIC_FLUSH_SECONDS` | `30` | | | ✓ | ✓ | | ✓ | ✓ | ✓ | Cadence of the background `_bulk` flusher that delivers queued reporting documents (run outcomes, experiment documents, engine exceptions, live trade audits, tracking deal documents); intervals with an empty buffer send nothing. A non-numeric or `< 1` value falls back to `30` with a logged warning |
| `OHLC_PREPOPULATE` | `1` | | | ✓ | | | ✓ | | | Seed OHLC **and range** bar histories from QuestDB at each symbol's first tick (`run`: replay start; `live`: launch — big-bar strategies trade immediately instead of warming up for days). One switch covers both bar types; set `0` to disable all warm-up queries |
| `INGEST_BIND_ADDR` | `127.0.0.1` | | | | | ✓ | | | | UDP bind address for the tick receiver |
| `INGEST_UDP_PORT` | `11111` | | | | | ✓ | | | | UDP bind port (overridable by the command's port argument) |
| `TRACKING_BIND_ADDR` | `127.0.0.1` | | | | | | | ✓ | | UDP bind address for the deal/trade-update receiver |
| `TRACKING_UDP_PORT` | `11112` | | | | | | | ✓ | | UDP bind port (overridable by the command's port argument) |
| `LIVE_BIND_ADDR` | `127.0.0.1` | | | | | | ✓ | | | UDP bind address for the live tick receiver |
| `LIVE_UDP_PORT` | `11110` | | | | | | ✓ | | | UDP bind port (overridable by the command's port argument) |
| `LIVE_MIN_SCORE` | `20` | | | | | | ✓ | | | Winner floor on `results.performanceScore` when selecting strategies from Elasticsearch (top 3 per (symbol, strategy) pair is fixed in code, not env-configurable) |
| `LIVE_MAX_DRAWDOWN_PERCENT` | `10` | | | | | | ✓ | | | Hard ceiling on `results.maxDrawdownPercent` (peak-to-trough giveback) when selecting winners — the Calmar half of `performanceScore` only blends drawdown in, so this gate is what actually excludes spiky runs |
| `LIVE_MIN_CALMAR_SCORE` | `30` | | | | | | ✓ | | | Floor on `results.calmarScore` when selecting winners (Calmar ratio ~2 on the score scale where ratio 3 = 50): growth must be ~2x the worst giveback. Complements the drawdown ceiling — the floor rejects smooth-but-stagnant runs, the ceiling rejects fast growers with deep absolute givebacks |
| `LIVE_TRADE_LOCK_SECONDS` | `30` | | | | | | ✓ | | | TTL of the per-(strategy, direction) Redis trade lock |
| `LIVE_TRACE_ENABLED` | `1` | | | | | | ✓ | | | Emit live-mode trace documents (order/close lifecycle, book sync, IG guard refusals, startup/shutdown, minutely stats) to the Elasticsearch index `live-traces` through the async batch publisher; set `0` to disable. Every document carries an `env` field (`TRADING_ENVIRONMENT`). Delivery still requires `ELASTIC_ENABLED=1` |
| `LIVE_LOG_SHIP_ENABLED` | `1` | | | | | | ✓ | | | Ship every engine log line (`logLine`/`error`) as a document to the Elasticsearch index `live-logs`; set `0` to disable. An independent kill switch from `LIVE_TRACE_ENABLED` — the narrative log and the structured trace events are separate feeds. Delivery still requires `ELASTIC_ENABLED=1` |
| `TRADING_ENVIRONMENT` | `demo` | | | | | | ✓ | ✓ | ✓ | IG environment; lowercased into the DynamoDB credentials key `Auth#<env>` (`demo`/`live`); `tracking` stamps it into each `live-trades` document's `env` field |

## AWS credentials (`live` and `positions`)

The IG session credentials are pulled from the DynamoDB table `MarketDataLive` (`source/shared/aws/dynamoAuth`). The engine reads no AWS variables itself — the AWS SDK's default credential/region chain applies:

- `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_DEFAULT_REGION` — or a configured profile / instance role.

Effectively **required** for `live` order placement and the `positions` sync (without a session, `positions` logs a warning and every cycle skips safely); there is no in-code default.

## Build and script variables

| Variable | Default | Read by | Purpose |
| --- | --- | --- | --- |
| `CC` / `CXX` | `clang` / `clang++` | `scripts/build.sh` (non-Homebrew path) | Pin a specific compiler, e.g. `CC=clang-20 CXX=clang++-20` |
| `ENABLE_COVERAGE` | `OFF` | `scripts/build.sh` | Instrument with Clang source-based coverage (CI turns it on for the SonarCloud report) |
| `CLEAN` | `0` | `scripts/test.sh` | `CLEAN=1` forces a clean reconfigure before the test build |

`scripts/run.sh` additionally **requires** `ELASTIC_HOST`, `ELASTIC_USER`, `ELASTIC_USER_PASSWORD`, and `REDIS_HOST` to be set and non-empty — it aborts up front if any are missing (the engine itself would fall back to the defaults above). It also pins `ELASTIC_TRADES_ENABLED=0` for the run it launches (per-trade indexing off).

## Secrets management

I manage secrets with [Infisical](https://infisical.com/), which injects them into the process environment at runtime:

```
infisical run -- bash ./scripts/run.sh
```

If you're not using Infisical, export the variables yourself (shell profile or a sourced `.env`) before invoking the scripts.
