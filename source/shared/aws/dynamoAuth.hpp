// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "shared/ig/igRestClient.hpp"  // ig_rest::Auth

// IG session credentials from DynamoDB, mirroring the C# engine's
// Auth.PullWithRetry over the AuthDyanmoDBObject table: an external login
// service keeps one item per trading environment refreshed with the current
// CST / X-SECURITY-TOKEN (they expire), and every engine pulls it fresh per
// broker request:
//
//   table "MarketDataLive", key { id: "Auth#<environment>", sort: "null" }
//   fields: url, apikey, CST, xSecurityToken (strings; the C# object's
//   date/authRoot are ignored here)
//
// AWS credentials and region come from the environment via the SDK's
// default chain (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY /
// AWS_DEFAULT_REGION, or a profile). This header is AWS-free (the SDK stays
// behind dynamoAuth.cpp), same isolation pattern as tradeLocks.hpp, so
// module global module fragments can #include it alongside `import std`.
namespace aws_auth {

inline constexpr std::string_view kAuthTable = "MarketDataLive";

// "Auth#<environment>" — the C# "Auth#" + TradingEnvironment key. The
// environment is expected lowercased ("live" / "demo").
std::string authKey(const std::string& environment);

// Pure item -> session mapping (exposed for tests): nullopt unless url,
// apikey, CST and xSecurityToken are all present and non-empty — a partial
// session is unusable and must read as "no auth".
std::optional<ig_rest::Auth> authFromItem(
    const std::map<std::string, std::string>& item);

// GetItem with ONE retry after 100ms on a transport/service error — the C#
// PullWithRetry. A missing item or an incomplete one is a definitive
// nullopt (no retry): the login service simply hasn't written a session for
// this environment.
std::optional<ig_rest::Auth> pullAuthWithRetry(const std::string& environment);

}  // namespace aws_auth
