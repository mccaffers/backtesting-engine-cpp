// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/aws/dynamoAuth.hpp"

#include <chrono>
#include <thread>

#include <aws/core/Aws.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/AttributeValue.h>
#include <aws/dynamodb/model/GetItemRequest.h>

#include "shared/utilities/backtestLog.hpp"

namespace {

// InitAPI must run once before any client exists; the matching ShutdownAPI
// runs at static destruction. Clients are constructed per pull (cheap next
// to the network hop, and never outliving the guard).
void ensureAwsInit() {
    static const struct AwsGlobal {
        Aws::SDKOptions options;
        AwsGlobal() { Aws::InitAPI(options); }
        ~AwsGlobal() { Aws::ShutdownAPI(options); }
    } guard;
    (void)guard;
}

// One GetItem. `retryable` distinguishes a transport/service error (worth
// the C# single retry) from a definitive miss (item absent/incomplete).
std::optional<ig_rest::Auth> pullOnce(const std::string& environment,
                                      bool& retryable) {
    retryable = false;
    ensureAwsInit();

    // Region + credentials resolve from the environment / profile via the
    // SDK's default chain.
    const Aws::Client::ClientConfiguration config;
    const Aws::DynamoDB::DynamoDBClient client(config);

    Aws::DynamoDB::Model::GetItemRequest request;
    request.SetTableName(Aws::String{aws_auth::kAuthTable});
    request.AddKey("id", Aws::DynamoDB::Model::AttributeValue().SetS(
                             aws_auth::authKey(environment).c_str()));
    // The C# AuthDyanmoDBObject range key defaults to the literal "null".
    request.AddKey("sort", Aws::DynamoDB::Model::AttributeValue().SetS("null"));

    const auto outcome = client.GetItem(request);
    if (!outcome.IsSuccess()) {
        retryable = true;
        backtest_log::error("DynamoAuth: GetItem "
                            + aws_auth::authKey(environment) + " failed: "
                            + std::string{outcome.GetError().GetMessage()});
        return std::nullopt;
    }

    const auto& item = outcome.GetResult().GetItem();
    if (item.empty()) {
        backtest_log::error("DynamoAuth: no session item for "
                            + aws_auth::authKey(environment)
                            + " (has the login service written one?)");
        return std::nullopt;
    }

    std::map<std::string, std::string> fields;
    for (const auto& [name, value] : item) {
        const Aws::String& s = value.GetS();  // empty for non-string attrs
        fields.emplace(std::string{name.c_str(), name.size()},
                       std::string{s.c_str(), s.size()});
    }
    auto auth = aws_auth::authFromItem(fields);
    if (!auth) {
        backtest_log::error("DynamoAuth: session item "
                            + aws_auth::authKey(environment)
                            + " is incomplete (need url, apikey, CST, "
                            "xSecurityToken)");
    }
    return auth;
}

}  // namespace

namespace aws_auth {

std::string authKey(const std::string& environment) {
    return "Auth#" + environment;
}

std::optional<ig_rest::Auth> authFromItem(
    const std::map<std::string, std::string>& item) {
    const auto field = [&item](const char* name) -> std::string {
        const auto it = item.find(name);
        return it == item.end() ? std::string{} : it->second;
    };
    ig_rest::Auth auth{
        .url = field("url"),
        .apiKey = field("apikey"),
        .cst = field("CST"),
        .xSecurityToken = field("xSecurityToken"),
    };
    if (auth.url.empty() || auth.apiKey.empty() || auth.cst.empty() ||
        auth.xSecurityToken.empty()) {
        return std::nullopt;
    }
    return auth;
}

std::optional<ig_rest::Auth> pullAuthWithRetry(const std::string& environment) {
    bool retryable = false;
    if (auto auth = pullOnce(environment, retryable)) {
        return auth;
    }
    if (!retryable) {
        return std::nullopt;
    }
    // The C# PullWithRetry: exactly one more attempt after 100ms.
    backtest_log::error("DynamoAuth: retrying credentials in 100ms");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    return pullOnce(environment, retryable);
}

}  // namespace aws_auth
