// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "run/reporting/elasticPublisher.hpp"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include "shared/utilities/backtestLog.hpp"
#include "shared/utilities/env.hpp"

namespace {

void ensureCurlInit() {
    static const struct CurlGlobal {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
        ~CurlGlobal() { curl_global_cleanup(); }
    } guard;
    (void)guard;
}

std::string generateUuid() {
    static thread_local boost::uuids::random_generator gen;
    return boost::uuids::to_string(gen());
}

// Swallow the response body so curl does not dump it to stdout (its default
// behaviour when no write callback is configured).
std::size_t discardResponse(char* /*ptr*/, std::size_t size, std::size_t nmemb,
                            void* /*userdata*/) {
    return size * nmemb;
}

// One PUT attempt. `code` follows putDocument's contract (0 ok, 1 curl init
// failure, 2 transport error, 3 non-2xx HTTP); `httpStatus` lets the caller
// decide whether a code-3 outcome is retryable (429/5xx) or permanent (4xx).
struct PutAttempt {
    int code;
    long httpStatus;
};

PutAttempt attemptPut(const std::string& url, const std::string& user,
                      const std::string& password, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        backtest_log::error("ElasticPublisher: curl_easy_init failed");
        return {1, 0};
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // HTTP basic auth when credentials are supplied via the environment.
    if (!user.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    // Bounded I/O so a hung Elastic endpoint cannot block a pool worker
    // forever (every publish runs on a ThreadPool task; an unbounded
    // curl_easy_perform would eventually fill every slot and stall the whole
    // drain loop). NOSIGNAL is required for libcurl on threads: without it,
    // resolver timeouts use SIGALRM/siglongjmp, which is not thread-safe.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // Discard the response body instead of letting curl print it to stdout.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);

    // Require TLS 1.2 or newer and enforce certificate / hostname verification
    // for any HTTPS endpoint (Sonar cpp:S4423 / S5527).
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        backtest_log::error(std::string("ElasticPublisher: PUT failed: ")
                            + curl_easy_strerror(rc));
        return {2, httpStatus};
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        backtest_log::error("ElasticPublisher: HTTP " + std::to_string(httpStatus)
                            + " from " + url);
        return {3, httpStatus};
    }
    return {0, httpStatus};
}

// Accumulate the response body into the caller's std::string. Unlike the PUT
// path (which discards responses), _bulk can return 200 with per-item failures
// inside the body, so the caller must be able to inspect it.
std::size_t captureResponse(char* ptr, std::size_t size, std::size_t nmemb,
                            void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// One attempt with a caller-chosen verb. Same bounded-I/O, auth and TLS setup
// as attemptPut (see the comments there); differs in caller-chosen content
// type (_bulk sends NDJSON, everything else JSON) and in capturing the
// response body — _bulk can return 200 with per-item failures, _search's
// whole point is the body, and index creation must inspect a 400 to tell
// "already exists" from a real rejection. For that reason a non-2xx status is
// NOT logged here: callers that treat some of them as benign log their own
// failures (attemptPost keeps the unconditional log for its callers).
PutAttempt attemptRequest(const char* method, const std::string& url,
                          const std::string& user, const std::string& password,
                          const std::string& body, const char* contentType,
                          std::string& responseBody) {
    responseBody.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        backtest_log::error("ElasticPublisher: curl_easy_init failed");
        return {1, 0};
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers, (std::string{"Content-Type: "} + contentType).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!user.empty()) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        backtest_log::error(std::string("ElasticPublisher: ") + method
                            + " failed: " + curl_easy_strerror(rc));
        return {2, httpStatus};
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        return {3, httpStatus};
    }
    return {0, httpStatus};
}

// One POST attempt — attemptRequest plus the unconditional non-2xx log every
// pre-existing caller (_bulk chunks, _search) relies on.
PutAttempt attemptPost(const std::string& url, const std::string& user,
                       const std::string& password, const std::string& body,
                       const char* contentType, std::string& responseBody) {
    const PutAttempt attempt = attemptRequest("POST", url, user, password, body,
                                              contentType, responseBody);
    if (attempt.code == 3) {
        backtest_log::error("ElasticPublisher: HTTP "
                            + std::to_string(attempt.httpStatus) + " from " + url);
    }
    return attempt;
}

// The one transient-failure retry policy shared by every HTTP entry point
// (putDocument, _bulk chunks, _search): up to kMaxRetryAttempts attempts;
// transport errors (code 2), HTTP 429 backpressure and 5xx are transient,
// everything else (other 4xx: mapping conflicts, auth) would fail identically
// every time and is permanent; back off 1s after the first failure, 3s after
// later ones.
constexpr int kMaxRetryAttempts = 3;

bool isTransientFailure(const PutAttempt& attempt) {
    return attempt.code == 2 ||
           (attempt.code == 3 &&
            (attempt.httpStatus == 429 || attempt.httpStatus >= 500));
}

void retryBackoff(const int attemptNumber) {
    std::this_thread::sleep_for(std::chrono::seconds(attemptNumber == 1 ? 1 : 3));
}

// Drives `tryOnce` through the policy above and returns the final attempt
// (code 0 on success). For requests whose whole outcome is one PutAttempt;
// postBulkChunk keeps its own loop because _bulk can partially fail per item.
PutAttempt attemptWithRetries(const std::function<PutAttempt()>& tryOnce) {
    PutAttempt attempt{};
    for (int i = 1; i <= kMaxRetryAttempts; ++i) {
        attempt = tryOnce();
        if (attempt.code == 0 || !isTransientFailure(attempt) ||
            i == kMaxRetryAttempts) {
            break;
        }
        retryBackoff(i);
    }
    return attempt;
}

// Append an undeliverable document as one NDJSON line so a sweep's computed
// results survive an Elastic outage and can be replayed later. `body` is
// already-serialised JSON, so it embeds verbatim. Serialised by a mutex:
// publishes happen concurrently on pool workers and lines must not interleave.
void deadLetter(const std::string& index, const std::string& docId,
                const std::string& body) {
    static std::mutex fileMutex;
    const std::string path =
        env::getOr("ELASTIC_DEADLETTER_PATH", "elastic_deadletter.ndjson");
    const std::lock_guard<std::mutex> lock{fileMutex};
    std::ofstream out{path, std::ios::app};
    if (!out) {
        backtest_log::error("ElasticPublisher: cannot open dead-letter file "
                            + path);
        return;
    }
    out << "{\"ts\":\"" << elastic::nowIsoUtc() << "\",\"index\":\"" << index
        << "\",\"docId\":\"" << docId << "\",\"doc\":" << body << "}\n";
}

// NDJSON body for one _bulk chunk: an action line naming the _id, then the
// pre-serialised document, one pair per doc. The trailing newline after the
// last line is mandatory — Elasticsearch rejects the request without it. The
// action line goes through nlohmann so the _id is JSON-escaped.
std::string buildBulkBody(const std::vector<const elastic::BulkDoc*>& docs) {
    std::size_t bytes = 0;
    for (const auto* doc : docs) {
        bytes += doc->id.size() + doc->body.size() + 32;
    }
    std::string body;
    body.reserve(bytes);
    for (const auto* doc : docs) {
        body += nlohmann::json{{"index", {{"_id", doc->id}}}}.dump();
        body += '\n';
        body += doc->body;
        body += '\n';
    }
    return body;
}

// POST one chunk (already within the doc/byte caps), retrying transient
// failures with the same backoff policy as putDocument. _bulk can partially
// fail — HTTP 2xx with "errors":true and a per-item status array that aligns
// 1:1 with the actions sent — so item-level 429/5xx rejections form a smaller
// retry batch while other 4xx (mapping conflicts, auth — they would fail
// identically every time) are dead-lettered immediately. The stable _ids make
// replaying a whole chunk an idempotent overwrite of any docs that DID land.
// Returns 0 when every doc in the chunk was accepted, non-zero otherwise.
int postBulkChunk(const std::string& index, const std::string& url,
                  const std::string& user, const std::string& password,
                  std::vector<const elastic::BulkDoc*> pending) {
    bool anyDeadLettered = false;
    std::string response;
    PutAttempt attempt{};
    for (int i = 1; i <= kMaxRetryAttempts; ++i) {
        attempt = attemptPost(url, user, password, buildBulkBody(pending),
                              "application/x-ndjson", response);

        if (attempt.code == 0) {
            std::vector<const elastic::BulkDoc*> retry;
            try {
                const nlohmann::json resp = nlohmann::json::parse(response);
                if (!resp.value("errors", false)) {
                    if (!backtest_log::is_quiet()) {
                        std::cout << "ElasticPublisher: bulk POST " << url
                                  << " (" << pending.size() << " docs, HTTP "
                                  << attempt.httpStatus << ")" << std::endl;
                    }
                    return anyDeadLettered ? 3 : 0;
                }
                const nlohmann::json& items = resp.at("items");
                for (std::size_t k = 0; k < pending.size(); ++k) {
                    long status = 0;
                    if (k < items.size() && items[k].is_object()) {
                        const nlohmann::json& action =
                            items[k].contains("index") ? items[k]["index"]
                                                       : items[k];
                        if (action.is_object()) {
                            status = action.value("status", 0);
                        }
                    }
                    if (status >= 200 && status < 300) {
                        continue;  // this doc landed
                    }
                    if (status == 429 || status >= 500) {
                        retry.push_back(pending[k]);
                    } else {
                        // Permanent (or unrecognisable) item failure: keep a
                        // replayable local copy rather than retrying a doc
                        // that would be rejected identically every time.
                        deadLetter(index, pending[k]->id, pending[k]->body);
                        anyDeadLettered = true;
                    }
                }
            } catch (const std::exception& e) {
                // 2xx but the response body defied inspection. Assume the
                // chunk was delivered: dead-lettering here would duplicate
                // docs that (most likely) landed.
                backtest_log::error(
                    std::string("ElasticPublisher: _bulk response inspection "
                                "failed (") + e.what()
                    + "); assuming chunk delivered");
                return anyDeadLettered ? 3 : 0;
            }
            if (retry.empty()) {
                // errors:true fully accounted for — everything either landed
                // or was dead-lettered above.
                if (anyDeadLettered) {
                    backtest_log::error("ElasticPublisher: _bulk to " + url
                                        + " rejected document(s); dead-lettered");
                }
                return anyDeadLettered ? 3 : 0;
            }
            pending = std::move(retry);
            if (i == kMaxRetryAttempts) {
                break;
            }
        } else {
            if (!isTransientFailure(attempt) || i == kMaxRetryAttempts) {
                break;
            }
        }
        retryBackoff(i);
    }

    // Retries exhausted or a permanent whole-request failure: same rationale
    // as putDocument — the backtest already ran, so keep replayable copies.
    backtest_log::error("ElasticPublisher: giving up on " + url
                        + " after retries; dead-lettering "
                        + std::to_string(pending.size()) + " document(s)");
    for (const auto* doc : pending) {
        deadLetter(index, doc->id, doc->body);
    }
    return attempt.code != 0 ? attempt.code : 3;
}

// Positive flush cadence from $ELASTIC_FLUSH_SECONDS. Best-effort like the
// rest of the publisher: junk falls back to the default with a logged warning
// instead of aborting the engine over a reporting knob.
std::chrono::seconds flushIntervalFromEnv() {
    const std::string raw = env::getOr("ELASTIC_FLUSH_SECONDS", "30");
    int value = 0;
    const auto [ptr, ec] =
        std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (ec != std::errc{} || ptr != raw.data() + raw.size() || value < 1) {
        backtest_log::error("ElasticPublisher: invalid ELASTIC_FLUSH_SECONDS '"
                            + raw + "', using 30");
        return std::chrono::seconds{30};
    }
    return std::chrono::seconds{value};
}

// Buffers documents per index and delivers them from one background thread in
// periodic _bulk batches, so a publish never blocks the thread that produced
// the document — a pool worker finishing a two-second backtest hands off its
// outcome docs and moves on; retries, backoff and dead-lettering all happen
// over here. Singleton via instance(): the magic static starts the thread on
// the first enqueue and its destructor (normal process exit) stops the thread
// and flushes the tail, so a direct `run` invocation's single result still
// lands before the process ends.
class DocumentBatcher {
public:
    static DocumentBatcher& instance() {
        // The constructor logs (flushIntervalFromEnv's invalid-value warning)
        // DURING the magic static's construction — before run()/flush() and
        // their guards exist. With the live-logs sink armed, that line would
        // re-enter instance() while the static is still initialising:
        // recursive static initialisation, which __cxa_guard_acquire turns
        // into an abort. Suppress around the construction; the guard costs
        // two thread_local writes on the (hot but cheap) steady-state path.
        const backtest_log::SinkSuppression suppression;
        static DocumentBatcher batcher;
        return batcher;
    }

    void enqueue(const std::string& index, elastic::BulkDoc doc) {
        const std::lock_guard<std::mutex> lock{mutex_};
        pending_[index].push_back(std::move(doc));
    }

    void enqueue(const std::string& index, std::vector<elastic::BulkDoc> docs) {
        const std::lock_guard<std::mutex> lock{mutex_};
        std::vector<elastic::BulkDoc>& queued = pending_[index];
        queued.insert(queued.end(), std::make_move_iterator(docs.begin()),
                      std::make_move_iterator(docs.end()));
    }

    // Swap the buffer out under the lock, deliver outside it — enqueues keep
    // landing (into the fresh buffer) while a flush is on the wire.
    int flush() {
        // flush also runs on caller threads (flushQueuedDocuments, the
        // destructor's tail) — its failure lines must not re-enter the queue
        // they describe, same doctrine as the no-putEngineException rule
        // below.
        const backtest_log::SinkSuppression suppression;
        std::map<std::string, std::vector<elastic::BulkDoc>> batch;
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (pending_.empty()) {
                return 0;
            }
            batch.swap(pending_);
        }
        int worst = 0;
        for (const auto& [index, docs] : batch) {
            // bulkIndexDocuments already logs and dead-letters every failure.
            // Deliberately NO putEngineException here: it enqueues into this
            // same batcher, so reporting a failed flush through it would feed
            // the next flush a fresh document for as long as the outage lasts.
            const int rc = elastic::bulkIndexDocuments(index, docs);
            if (rc != 0) {
                worst = rc;
            }
        }
        return worst;
    }

    ~DocumentBatcher() {
        // Static teardown begins here: any later log line shipping through
        // the sink would enqueue into this dying batcher. Disarm first so
        // other statics' destructors can keep logging to stderr safely.
        backtest_log::setSink(nullptr);
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            stop_ = true;
        }
        cv_.notify_all();
        flusher_.join();
        flush();  // the tail: anything enqueued after the thread's last pass
    }

    DocumentBatcher(const DocumentBatcher&) = delete;
    DocumentBatcher& operator=(const DocumentBatcher&) = delete;

private:
    DocumentBatcher() : interval_(flushIntervalFromEnv()) {
        // Complete curl's global-init guard before this constructor returns:
        // statics destroy in reverse order of construction, so curl teardown
        // then happens AFTER ~DocumentBatcher's tail flush.
        ensureCurlInit();
        flusher_ = std::thread([this] { run(); });
    }

    void run() {
        // The flusher's own log lines must never re-enter this queue (see
        // flush() below) — suppress the live-logs sink for this thread's
        // whole lifetime.
        const backtest_log::SinkSuppression suppression;
        std::unique_lock<std::mutex> lock{mutex_};
        for (;;) {
            cv_.wait_for(lock, interval_, [this] { return stop_; });
            if (stop_) {
                return;  // the destructor flushes the tail after the join
            }
            if (pending_.empty()) {
                continue;  // quiet interval — no POST for an empty buffer
            }
            lock.unlock();
            flush();
            lock.lock();
        }
    }

    const std::chrono::seconds interval_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::map<std::string, std::vector<elastic::BulkDoc>> pending_;
    std::thread flusher_;
};

}  // namespace

namespace elastic {

std::string nowIsoUtc() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm_buf{};
    gmtime_r(&now, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string{buf};
}

int putDocument(const std::string& index, const std::string& body,
                const std::string& docId) {
    // Publisher-origin failure lines must not ship through the live-logs
    // sink back into this publisher (see backtestLog.hpp).
    const backtest_log::SinkSuppression suppression;
    // Allow runs to opt out of result reporting entirely (e.g. local backtests
    // with no Elastic instance). On by default to preserve existing behaviour.
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return 0;
    }

    ensureCurlInit();

    const std::string host = env::getOr("ELASTIC_HOST", "http://localhost:9200");
    const std::string user = env::getOr("ELASTIC_USER", "");
    const std::string password = env::getOr("ELASTIC_USER_PASSWORD", "");
    const std::string id = docId.empty() ? generateUuid() : docId;
    const std::string url = host + "/" + index + "/_doc/" + id;

    // Retry transient failures (attemptWithRetries). A caller-supplied docId
    // makes the replayed PUT idempotent — it overwrites the same _id rather
    // than duplicating the document.
    const PutAttempt attempt = attemptWithRetries(
        [&] { return attemptPut(url, user, password, body); });
    if (attempt.code == 0) {
        // Per-strategy success line is skipped under concurrent backtests.
        if (!backtest_log::is_quiet()) {
            std::cout << "ElasticPublisher: PUT " << url << " (HTTP "
                      << attempt.httpStatus << ")" << std::endl;
        }
        return 0;
    }

    // The backtest that produced this document already ran; losing the doc
    // would silently skew the sweep's accounting (results/failures/final
    // counts drifting apart). Keep a replayable local copy instead.
    backtest_log::error("ElasticPublisher: giving up on " + url
                        + " after retries; dead-lettering document");
    deadLetter(index, id, body);
    return attempt.code;
}

int bulkIndexDocuments(const std::string& index,
                       const std::vector<BulkDoc>& docs) {
    // Same rationale as putDocument: delivery failures log, and those lines
    // must not re-enter the queue they describe.
    const backtest_log::SinkSuppression suppression;
    if (docs.empty()) {
        return 0;
    }
    // Same opt-out as putDocument: no Elastic instance, no reporting.
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return 0;
    }

    ensureCurlInit();

    const std::string host = env::getOr("ELASTIC_HOST", "http://localhost:9200");
    const std::string user = env::getOr("ELASTIC_USER", "");
    const std::string password = env::getOr("ELASTIC_USER_PASSWORD", "");
    const std::string url = host + "/" + index + "/_bulk";

    // Bound each request so a trade-heavy run cannot produce an oversized
    // _bulk body (Elasticsearch's sweet spot is single-digit megabytes); the
    // doc cap keeps the per-item response array cheap to walk. Chunks POST
    // sequentially — this runs once at end of run, off the hot path.
    constexpr std::size_t kMaxBulkDocs = 1000;
    constexpr std::size_t kMaxBulkBytes = 4 * 1024 * 1024;

    int worst = 0;
    std::vector<const BulkDoc*> chunk;
    std::size_t chunkBytes = 0;
    const auto flush = [&] {
        if (chunk.empty()) {
            return;
        }
        const int rc = postBulkChunk(index, url, user, password, chunk);
        if (rc != 0) {
            worst = rc;
        }
        chunk.clear();
        chunkBytes = 0;
    };
    for (const BulkDoc& doc : docs) {
        chunk.push_back(&doc);
        chunkBytes += doc.id.size() + doc.body.size() + 32;  // + action line
        if (chunk.size() >= kMaxBulkDocs || chunkBytes >= kMaxBulkBytes) {
            flush();
        }
    }
    flush();
    return worst;
}

void enqueueDocument(const std::string& index, const std::string& body,
                     const std::string& docId) {
    // Same opt-out as putDocument — checked at enqueue time so a disabled run
    // never buffers anything (or starts the flusher thread at all).
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return;
    }
    DocumentBatcher::instance().enqueue(
        index, {docId.empty() ? generateUuid() : docId, body});
}

void enqueueDocuments(const std::string& index, std::vector<BulkDoc> docs) {
    if (docs.empty() || env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return;
    }
    DocumentBatcher::instance().enqueue(index, std::move(docs));
}

int flushQueuedDocuments() {
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return 0;
    }
    return DocumentBatcher::instance().flush();
}

int searchIndex(const std::string& index, const std::string& queryBody,
                std::string& responseBody, long& httpStatus) {
    responseBody.clear();
    httpStatus = 0;

    // Publisher-origin lines stay out of the live-logs sink here too — the
    // caller's own logging (e.g. liveWinners' failure lines) still ships.
    const backtest_log::SinkSuppression suppression;

    // A disabled reporter has nothing to read from — surface it as a failure
    // (unlike the write paths, where "disabled" means a successful no-op).
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        backtest_log::error(
            "ElasticPublisher: _search requested but ELASTIC_ENABLED=0");
        return 4;
    }

    ensureCurlInit();

    const std::string host = env::getOr("ELASTIC_HOST", "http://localhost:9200");
    const std::string user = env::getOr("ELASTIC_USER", "");
    const std::string password = env::getOr("ELASTIC_USER_PASSWORD", "");
    const std::string url = host + "/" + index + "/_search";

    // Same transient-failure policy as putDocument, minus the dead-letter:
    // a failed read leaves nothing to replay, the caller just re-queries.
    // responseBody ends up holding the final attempt's body either way
    // (attemptPost clears it per attempt).
    const PutAttempt attempt = attemptWithRetries([&] {
        return attemptPost(url, user, password, queryBody, "application/json",
                           responseBody);
    });
    httpStatus = attempt.httpStatus;
    return attempt.code;
}

int ensureIndexExists(const std::string& index) {
    const backtest_log::SinkSuppression suppression;
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return 0;
    }

    ensureCurlInit();

    const std::string host = env::getOr("ELASTIC_HOST", "http://localhost:9200");
    const std::string user = env::getOr("ELASTIC_USER", "");
    const std::string password = env::getOr("ELASTIC_USER_PASSWORD", "");
    const std::string url = host + "/" + index;

    std::string response;
    const PutAttempt attempt = attemptWithRetries([&] {
        return attemptRequest("PUT", url, user, password, "{}",
                              "application/json", response);
    });
    if (attempt.code == 0) {
        if (!backtest_log::is_quiet()) {
            std::cout << "ElasticPublisher: created index " << index
                      << std::endl;
        }
        return 0;
    }
    // The weekly load PUTs the same index name on every invocation; the index
    // already being there is the desired state, not a failure.
    if (attempt.httpStatus == 400 &&
        response.find("resource_already_exists_exception") != std::string::npos) {
        return 0;
    }
    backtest_log::error("ElasticPublisher: creating index " + index
                        + " failed (HTTP " + std::to_string(attempt.httpStatus)
                        + "): " + response.substr(0, 500));
    return attempt.code;
}

int repointAlias(const std::string& alias, const std::string& index) {
    const backtest_log::SinkSuppression suppression;
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return 0;
    }

    ensureCurlInit();

    const std::string host = env::getOr("ELASTIC_HOST", "http://localhost:9200");
    const std::string user = env::getOr("ELASTIC_USER", "");
    const std::string password = env::getOr("ELASTIC_USER_PASSWORD", "");
    const std::string url = host + "/_aliases";

    // Built field-by-field: nested brace-init of nlohmann objects is ambiguous
    // between object and array forms.
    nlohmann::json remove;
    remove["remove"]["index"] = "*";
    remove["remove"]["alias"] = alias;
    remove["remove"]["must_exist"] = false;
    nlohmann::json add;
    add["add"]["index"] = index;
    add["add"]["alias"] = alias;
    nlohmann::json actions;
    actions["actions"] = nlohmann::json::array({remove, add});

    std::string response;
    const PutAttempt attempt = attemptWithRetries([&] {
        return attemptPost(url, user, password, actions.dump(),
                           "application/json", response);
    });
    if (attempt.code != 0) {
        backtest_log::error("ElasticPublisher: repointing alias " + alias
                            + " -> " + index + " failed (HTTP "
                            + std::to_string(attempt.httpStatus) + "): "
                            + response.substr(0, 500));
        return attempt.code;
    }
    if (!backtest_log::is_quiet()) {
        std::cout << "ElasticPublisher: alias " << alias << " -> " << index
                  << std::endl;
    }
    return 0;
}

void putEngineException(const EngineException& ex) noexcept {
    // Callers report from inside catch handlers, so this must never throw back
    // out. nlohmann's dump() throws type_error.316 on invalid UTF-8, and the
    // message is arbitrary exception text — guard it explicitly.
    try {
        enqueueDocument("engine_exceptions", nlohmann::json(ex).dump());
    } catch (const std::exception& e) {
        backtest_log::error(
            std::string("ElasticPublisher: failed to report engine exception: ")
            + e.what());
    } catch (...) {
    }
}

}  // namespace elastic
