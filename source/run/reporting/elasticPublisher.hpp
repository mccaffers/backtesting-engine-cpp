// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>
#include <vector>

#include "run/reporting/engineException.hpp"

// Minimal Elasticsearch HTTP client — indexing (PUT/_bulk) for run outcomes
// and engine exceptions, a _search read path (live winner selection), and
// index/alias admin for the weekly outcome indices (load; outcomeIndices.hpp).
// Host is read from $ELASTIC_HOST (default http://localhost:9200) with
// optional HTTP basic auth from $ELASTIC_USER / $ELASTIC_USER_PASSWORD.
// Reporting can be disabled entirely with $ELASTIC_ENABLED=0.
//
// Two write paths: putDocument/bulkIndexDocuments deliver synchronously on the
// calling thread; enqueueDocument(s) hand the document to a background flusher
// that batches everything into periodic _bulk requests, so the caller never
// blocks on the network. Report-only documents (run outcomes, engine
// exceptions, live trade audits) go through the queue — fast sweeps were
// producing one PUT per finished run, a request storm Elasticsearch answered
// with 429s.
//
// Lives in shared/ (as a plain header, not a module) so both the run path
// (elasticClient) and the shared redis-consumer path (redisRunner, drainRuns,
// which import nothing) can report through one implementation.
namespace elastic {

// PUT one JSON document into `index`. `docId` names the Elasticsearch _id so
// retries are idempotent (a replayed PUT overwrites the same document instead
// of duplicating it); when empty, a fresh UUID is generated. Transient
// failures (transport errors, HTTP 429/5xx) are retried with backoff; a
// document that still cannot be delivered is appended to a local NDJSON
// dead-letter file ($ELASTIC_DEADLETTER_PATH, default
// elastic_deadletter.ndjson) for later replay.
// Returns 0 on success (or when reporting is disabled), non-zero otherwise.
int putDocument(const std::string& index, const std::string& body,
                const std::string& docId = "");

// ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ").
std::string nowIsoUtc();

// One document destined for a _bulk request: the deterministic _id plus the
// pre-serialised JSON body. The id must be non-empty — bulk retries replay
// whole chunks, so only a stable _id keeps them idempotent overwrites.
struct BulkDoc {
    std::string id;
    std::string body;
};

// POST `docs` into `index` via the Elasticsearch _bulk API, in chunks (doc- and
// byte-bounded). Same env config, retry/backoff and dead-letter behaviour as
// putDocument; documents rejected item-by-item inside an otherwise-successful
// bulk response (mapping conflicts, etc.) are dead-lettered too. Returns 0 when
// every document was accepted (or reporting is disabled), non-zero when any
// document had to be dead-lettered.
int bulkIndexDocuments(const std::string& index, const std::vector<BulkDoc>& docs);

// Queue one JSON document for the background flusher and return immediately —
// no network I/O on the calling thread. A dedicated thread delivers everything
// queued (across all indices) through bulkIndexDocuments — same retry/backoff
// and dead-letter treatment — every $ELASTIC_FLUSH_SECONDS (default 30),
// skipping intervals where nothing is buffered; a final flush runs at normal
// process exit. The trade-off: documents ride in memory for up to one
// interval, so an abnormal termination (signal, crash) loses that tail —
// nothing is dead-lettered for documents never attempted. `docId` as in
// putDocument; when empty a UUID is minted at enqueue time (the _bulk path
// needs a stable _id to keep retries idempotent).
void enqueueDocument(const std::string& index, const std::string& body,
                     const std::string& docId = "");

// Queue a pre-built batch for `index` in one lock acquisition (the per-trade
// documents arrive hundreds at a time). Same delivery semantics as
// enqueueDocument; every doc must already carry a non-empty id.
void enqueueDocuments(const std::string& index, std::vector<BulkDoc> docs);

// Deliver everything currently queued, synchronously, on the calling thread.
// The periodic flusher and process exit call this automatically; exposed for
// callers that need documents visible before the next interval. Returns 0 when
// every document was accepted (or nothing was queued / reporting disabled).
int flushQueuedDocuments();

// POST `queryBody` to {ELASTIC_HOST}/{index}/_search and capture the response
// body. Read path: transient failures (transport errors, HTTP 429/5xx) are
// retried with the same backoff as putDocument, but nothing is dead-lettered —
// there is no document to replay; the caller simply re-queries. Returns 0 on
// HTTP 2xx; otherwise the attempt code (1 curl init, 2 transport, 3 non-2xx
// HTTP, 4 reporting disabled via $ELASTIC_ENABLED=0). `httpStatus` is the last
// HTTP status seen (0 when no HTTP exchange happened) so callers can tell a
// permanent 4xx (bad query / mapping) from an outage.
int searchIndex(const std::string& index, const std::string& queryBody,
                std::string& responseBody, long& httpStatus);

// PUT {ELASTIC_HOST}/{index} — create the index if it does not exist yet.
// Elasticsearch answering HTTP 400 resource_already_exists_exception counts
// as success: the load command re-runs this for every invocation in a week
// and the index already being there is exactly the state it wants. Same
// transient-failure retry policy as putDocument (nothing to dead-letter).
// Returns 0 on success or when reporting is disabled via $ELASTIC_ENABLED=0.
int ensureIndexExists(const std::string& index);

// Atomically repoint `alias` at exactly `index` via one POST /_aliases action
// set: remove the alias from EVERY index currently holding it (must_exist:
// false, so the very first swap — and a hand-parked bootstrap alias on a
// legacy index — are handled), then add it to `index`. Elasticsearch applies
// the set atomically, so readers never observe the alias missing or doubled.
// `index` must already exist (ensureIndexExists). Returns 0 on success or
// when reporting is disabled.
int repointAlias(const std::string& alias, const std::string& index);

// Convenience: serialise and queue an engine exception for "engine_exceptions"
// (enqueueDocument semantics — delivered by the background flusher). noexcept
// by contract — every caller invokes this from a catch handler where a throw
// would mask the original failure (and, on a worker, abort the drain loop).
// Serialisation errors are logged and swallowed.
void putEngineException(const EngineException& ex) noexcept;

}  // namespace elastic
