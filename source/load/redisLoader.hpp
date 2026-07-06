// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <cstddef>
#include <string>
#include <vector>

// LPUSH pairs with RedisRunner's RPOP so consumers observe FIFO ordering.
class RedisLoader {
public:
    // One strategy destined for its own Redis string key: `key` is the full
    // payload key name (see queue_keys::strategyPayloadKey) and `rawJson` the
    // strategy JSON, Base64-encoded before storage.
    struct KeyedPayload {
        std::string key;
        std::string rawJson;
    };

    // Pull interface for loadKeyedPayloadStream: next() returns the next chunk
    // of payloads to store, or an empty vector when the sweep is exhausted.
    // Called on the loader's coroutine between Redis writes, so producing a
    // chunk lazily bounds memory at one chunk regardless of grid size.
    class ChunkSource {
    public:
        virtual ~ChunkSource() = default;
        virtual std::vector<KeyedPayload> next() = 0;
    };

    // LPUSHes a single Base64-encoded payload onto queueKey without assuming a
    // payload type (run descriptor or strategy).
    static int loadPayload(const std::string& redisHost,
                           int redisPort,
                           const std::string& queueKey,
                           const std::string& rawJson);

    // Streams every chunk from `source` over ONE connection. Per chunk, one
    // pipelined SET (with ttlSeconds expiry) per payload key, then the key
    // NAMES are LPUSHed onto listKey — in that order, so a consumer can never
    // pop a name whose payload is not yet stored. The TTL is a safety net for
    // crashed/abandoned runs; the consumer's GETDEL is the normal cleanup.
    static int loadKeyedPayloadStream(const std::string& redisHost,
                                      int redisPort,
                                      const std::string& listKey,
                                      ChunkSource& source,
                                      long ttlSeconds);
};
