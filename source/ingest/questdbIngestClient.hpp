// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Writes ticks into QuestDB using the InfluxDB Line Protocol over HTTP
// (POST http://<host>:<port>/write), reusing the project's existing libcurl
// dependency (mirrors elasticPublisher). Lines are queued from the receive
// thread and flushed in batches by an internal worker thread, so a slow QuestDB
// never stalls the UDP receive loop.
//
// The libcurl implementation and the worker thread live entirely in the .cpp
// (pimpl), keeping this header light enough to #include from a module's global
// module fragment.
namespace ingest {

class QuestdbIngestClient {
public:
    // host/port address the QuestDB ILP-over-HTTP endpoint. batchSize caps how
    // many lines go in one POST; flushInterval bounds how long a partial batch
    // waits before being sent. maxQueue caps how many unsent lines are buffered
    // when QuestDB is slow/down — past it the oldest lines are dropped so memory
    // stays bounded rather than growing without limit.
    explicit QuestdbIngestClient(std::string host,
                                 std::uint16_t port = 9001,
                                 std::size_t batchSize = 1000,
                                 std::chrono::milliseconds flushInterval =
                                     std::chrono::milliseconds(100),
                                 std::size_t maxQueue = 1'000'000);
    ~QuestdbIngestClient();

    QuestdbIngestClient(const QuestdbIngestClient&) = delete;
    QuestdbIngestClient& operator=(const QuestdbIngestClient&) = delete;

    // Queue one ready-to-send ILP line (must already end in '\n'). Thread-safe
    // and non-blocking; the worker thread does the actual HTTP POST. If the queue
    // is at capacity the oldest queued line is dropped to make room.
    void enqueueLine(std::string ilpLine);

    // Total lines dropped so far because the queue was full (QuestDB couldn't
    // keep up). Thread-safe.
    [[nodiscard]] std::size_t droppedLines() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ingest
