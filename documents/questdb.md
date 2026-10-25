# QuestDB

Tick storage for the engine: `ingest` writes, `run`/`analysis` read, `live` reads for OHLC/range-bar warm-up (see the runtime-services table in [REQUIREMENTS.md](../REQUIREMENTS.md)).

## Install & launch

The command below is the author's setup — a local install under `$HOME/dev/questdb` with Apple-Silicon Homebrew OpenJDK 17:

```
JAVA_HOME="/opt/homebrew/opt/openjdk@17" sh $HOME/dev/questdb/questdb.sh start -d $HOME/dev/questdb/data
```

On a fresh machine any stock QuestDB works: `brew install questdb`, the release tarball, or Docker (`docker run -p 9000:9000 -p 8812:8812 questdb/questdb`).

## Ports & credentials

- **Writes**: ILP-over-HTTP on `9000` (`$QUESTDB_ILP_PORT`) — the `ingest` command (`source/ingest/ingestCommand.cppm`), batched by `source/ingest/questdbIngestClient.hpp`.
- **Reads**: pgwire on `8812` (`$QUESTDB_PORT`) — `source/shared/questdb/connectionFactory.cppm` builds the connection from the environment with the stock QuestDB credentials: dbname `qdb`, user `admin`, password `quest`.

Host comes from `$QUESTDB_HOST` (the `run` command can also take it as an argument) — see [ENVIRONMENT.md](../ENVIRONMENT.md) for all three variables.

## Schema contract

One table per symbol, table name == symbol (`EURUSD`, ...):

- `ask`, `bid` — scaled fixed-point INT32 "points", **not** floats: the real price times the symbol's multiplier (FX majors ×100000, JPY pairs & metals ×1000, indices/commodities ×100 — `source/shared/models/priceData.cppm`, `source/shared/utilities/symbolScale.cppm`). e.g. EURUSD 1.10001 is stored as 110001.
- designated `timestamp` — written in nanoseconds, the QuestDB HTTP default precision.
- `source` — `SYMBOL`, the feed the tick came from: `IG` (IG Markets) or `MX` (Massive, formerly Polygon). Rows from before source tracking, and any tick with an empty or unrecognised source, are `NULL`.

The write side emits ILP lines of the form `{symbol},source={IG|MX} ask={..}i,bid={..}i {tsNanos}` (`source/ingest/tickLine.cppm`), omitting the `source` tag when the feed id isn't one of the two known values. The read side names columns explicitly and parses positionally as (symbol, ask, bid, timestamp) (`source/shared/questdb/sqlManager.cppm`), so tables must serve those columns; `source` is not read yet, so backtests see both feeds' ticks together.

Symbols are whitelisted: `SqlManager` throws `std::invalid_argument` for any symbol not in the `symbolScale` table, so a table named outside the whitelist is unreadable by the engine.

### How the `source` column appears

- `source` must stay an ILP **tag**, which QuestDB stores as `SYMBOL`. If it were ever sent as a field (`source="IG"`), a new table would get a string column and every later tagged write to it would fail (counted as `post-failed`).
- QuestDB adds the column to an existing table on that table's first tagged line; no migration is needed. Existing rows are not rewritten and read as `NULL`. Backfilling them with `UPDATE` would rewrite every affected partition, so it is slow on large tables.
- A table created from scratch by `ingest` gets `source` from its first line.

## Reachability

`scripts/ingest.sh` probes `http://$QUESTDB_HOST:$QUESTDB_ILP_PORT/exec?query=SELECT%201` before starting — non-fatal, since the ingest writer buffers (and eventually sheds) while QuestDB is down.
