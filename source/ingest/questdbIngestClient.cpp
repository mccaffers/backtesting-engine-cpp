// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// QuestDB ILP-over-HTTP writer. libcurl and the batching worker thread are
// confined to this classic translation unit (no `import std`), so curl's
// C-macro pollution never reaches the module boundary — the same isolation
// elasticPublisher.cpp uses for the Elasticsearch client.

#include "ingest/questdbIngestClient.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include <curl/curl.h>

#include "shared/utilities/backtestLog.hpp"

namespace {

// Bound how long a single POST can block, so a slow/black-holed QuestDB can't
// wedge the worker thread (and therefore shutdown, which joins it) indefinitely.
constexpr long kConnectTimeoutSeconds = 5;
constexpr long kTransferTimeoutSeconds = 10;

void ensureCurlInit() {
    static const struct CurlGlobal {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
        ~CurlGlobal() { curl_global_cleanup(); }
    } guard;
    (void)guard;
}

// Swallow the response body so curl does not dump it to stdout (its default when
// no write callback is set). QuestDB replies 204 on success, 400 + a JSON body
// describing the offending line on a parse/type error.
std::size_t discardResponse(char* /*ptr*/, std::size_t size, std::size_t nmemb,
                            void* /*userdata*/) {
    return size * nmemb;
}

}  // namespace

namespace ingest {

struct QuestdbIngestClient::Impl {
    std::string url;                       // http://host:port/write
    std::size_t batchSize;
    std::chrono::milliseconds flushInterval;
    std::size_t maxQueue;

    std::mutex mtx;
    std::condition_variable cv;            // plain CV (not _any): avoids the
                                           // import-std condition_variable_any
                                           // link bug, and this is a classic TU
    std::deque<std::string> queue;
    std::size_t droppedTotal = 0;          // guarded by mtx

    CURL* curl = nullptr;                  // owned by the worker thread only
    std::jthread worker;

    Impl(std::string host, std::uint16_t port, std::size_t batch,
         std::chrono::milliseconds interval, std::size_t maxQueueLines)
        : url("http://" + std::move(host) + ":" + std::to_string(port) + "/write"),
          batchSize(batch),
          flushInterval(interval),
          maxQueue(maxQueueLines) {
        ensureCurlInit();
        curl = curl_easy_init();
        if (!curl) {
            backtest_log::error("QuestdbIngestClient: curl_easy_init failed");
        }
        worker = std::jthread([this](std::stop_token st) { workerLoop(st); });
    }

    ~Impl() {
        worker.request_stop();
        cv.notify_all();
        worker.join();         // drain the queue before tearing down curl
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }

    void workerLoop(std::stop_token st) {
        while (!st.stop_requested()) {
            {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, flushInterval,
                            [&] { return !queue.empty() || st.stop_requested(); });
            }
            drainAndPost();
        }
        // Final drain after stop so queued ticks are not lost on shutdown. One
        // batch per POST keeps each body bounded by the transfer timeout.
        while (drainAndPost()) {
        }
    }

    // Move up to batchSize lines off the queue and POST them. Returns false when
    // the queue was empty (nothing sent).
    bool drainAndPost() {
        std::string batch;
        {
            std::unique_lock lock(mtx);
            const std::size_t n = std::min(queue.size(), batchSize);
            for (std::size_t i = 0; i < n; ++i) {
                batch += std::move(queue.front());
                queue.pop_front();
            }
        }
        if (batch.empty()) {
            return false;
        }
        post(batch);
        return true;
    }

    void post(const std::string& body) {
        if (!curl) {
            return;  // init failed earlier; error already logged
        }
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
        // Bound blocking time so a dead/slow QuestDB can't wedge the worker.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTransferTimeoutSeconds);

        const CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            backtest_log::error(std::string("QuestdbIngestClient: POST failed: ")
                                + curl_easy_strerror(rc));
            return;
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300) {
            backtest_log::error("QuestdbIngestClient: HTTP " + std::to_string(status)
                                + " from " + url);
        }
    }
};

QuestdbIngestClient::QuestdbIngestClient(std::string host, std::uint16_t port,
                                         std::size_t batchSize,
                                         std::chrono::milliseconds flushInterval,
                                         std::size_t maxQueue)
    : impl_(std::make_unique<Impl>(std::move(host), port, batchSize, flushInterval,
                                   maxQueue)) {}

QuestdbIngestClient::~QuestdbIngestClient() = default;

void QuestdbIngestClient::enqueueLine(std::string ilpLine) {
    bool wake = false;
    std::size_t dropped = 0;
    {
        std::scoped_lock lock(impl_->mtx);
        if (impl_->queue.size() >= impl_->maxQueue) {
            // Queue is full (QuestDB can't keep up). Drop the oldest line to keep
            // memory bounded and retain the freshest ticks.
            impl_->queue.pop_front();
            dropped = ++impl_->droppedTotal;
        }
        impl_->queue.push_back(std::move(ilpLine));
        wake = impl_->queue.size() >= impl_->batchSize;
    }
    // Warn on the first drop and then sparsely, so a sustained overload doesn't
    // flood the log. Done outside the lock.
    if (dropped == 1 || (dropped != 0 && dropped % 100000 == 0)) {
        backtest_log::error("QuestdbIngestClient: queue full (cap "
                            + std::to_string(impl_->maxQueue)
                            + "), dropping oldest lines; total dropped="
                            + std::to_string(dropped));
    }
    if (wake) {
        impl_->cv.notify_one();  // a full batch is ready — flush now
    }
}

std::size_t QuestdbIngestClient::droppedLines() const {
    std::scoped_lock lock(impl_->mtx);
    return impl_->droppedTotal;
}

}  // namespace ingest
