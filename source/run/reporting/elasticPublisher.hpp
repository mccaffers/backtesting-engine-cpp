// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <string>

#include "run/reporting/engineException.hpp"

// Minimal Elasticsearch HTTP client — PUT-only, for indexing run outcomes and
// engine exceptions. Host is read from $ELASTIC_HOST (default
// http://localhost:9200) with optional HTTP basic auth from $ELASTIC_USER /
// $ELASTIC_USER_PASSWORD. Reporting can be disabled entirely with
// $ELASTIC_ENABLED=0.
//
// Lives in shared/ (as a plain header, not a module) so both the run path
// (elasticClient) and the shared redis-consumer path (redisRunner, drainRuns,
// which import nothing) can report through one implementation.
namespace elastic {

// PUT one JSON document into `index`, with a freshly generated UUID id.
// Returns 0 on success (or when reporting is disabled), non-zero otherwise.
int putDocument(const std::string& index, const std::string& body);

// ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ").
std::string nowIsoUtc();

// Convenience: serialise and PUT an engine exception into "engine_exceptions".
// noexcept by contract — every caller invokes this from a catch handler where a
// throw would mask the original failure (and, on a worker, abort the drain
// loop). Serialisation/transport errors are logged and swallowed.
int putEngineException(const EngineException& ex) noexcept;

}  // namespace elastic
