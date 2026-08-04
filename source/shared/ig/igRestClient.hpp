// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// Minimal IG REST transport, mirroring the C# engine's IGMarketRequests
// executor: one bounded, retried HTTPS exchange with the IG session headers
// attached. No broker/business logic lives here — request gating (rate
// limit, duplicate suppression) is the caller's job (see the igRequests
// module) and response interpretation belongs to igMarkets.
//
// Same isolation pattern as elasticPublisher: plain header/.cpp pair (curl
// stays out of module global module fragments), Asio-free header.
namespace ig_rest {

// One IG session's credentials. Pulled per request from DynamoDB (see
// shared/aws/dynamoAuth), where an external login service keeps the
// expiring CST / X-SECURITY-TOKEN pair refreshed.
struct Auth {
    std::string url;             // account API base, e.g. https://api.ig.com/gateway/deal
    std::string apiKey;          // X-IG-API-KEY
    std::string cst;             // CST session header
    std::string xSecurityToken;  // X-SECURITY-TOKEN session header
};

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    long status{};
    std::string body;
};

// IG's per-endpoint Version header: "1" when the caller is tunnelling a
// DELETE through POST (an extra "_method: DELETE" header — IG's close-
// position API), otherwise "2". Free function so tests can pin the rule.
std::string versionFor(const Headers& extraHeaders);

// True when `headers` already carries `name` (case-insensitive). execute()
// uses this to let a caller-supplied Version header (e.g. the confirms
// endpoint, which is Version 1 without being a tunnelled DELETE) REPLACE
// the versionFor default instead of duplicating it on the wire.
bool hasHeader(const Headers& headers, std::string_view name);

// One exchange with retries: transient outcomes (transport error, HTTP 5xx
// or 408) are retried maxRetries times with exponential backoff (2s, 4s —
// the C# Polly policy at the default RETRY_COUNT=2), each attempt bounded by
// a 30s timeout. Returns the final response — INCLUDING a non-2xx one (the
// caller interprets status) — or nullopt when the final attempt still had no
// HTTP exchange at all. maxRetries = 0 sends exactly one attempt: required
// for the NON-IDEMPOTENT open POST, where a blind re-send after a lost ACK
// could double a live position (the caller resolves ambiguity through the
// confirms endpoint instead — see igRequests makeOpen). `extraHeaders` are
// sent verbatim (this is how "_method: DELETE" reaches IG) on top of the
// session headers, Version, Accept and Content-Type.
std::optional<HttpResponse> execute(const Auth& auth, const std::string& path,
                                    const std::string& method,
                                    const std::string& jsonBody,
                                    const Headers& extraHeaders = {},
                                    int maxRetries = 2);

}  // namespace ig_rest
