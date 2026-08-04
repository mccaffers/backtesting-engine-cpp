// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/ig/igRestClient.hpp"

#include <cctype>
#include <chrono>
#include <string>
#include <thread>

#include <curl/curl.h>

#include "shared/utilities/backtestLog.hpp"

namespace {

void ensureCurlInit() {
    static const struct CurlGlobal {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
        ~CurlGlobal() { curl_global_cleanup(); }
    } guard;
    (void)guard;
}

std::size_t captureResponse(char* ptr, std::size_t size, std::size_t nmemb,
                            void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

bool caseInsensitiveEquals(const std::string_view a, const std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// One attempt. nullopt = no HTTP exchange happened (curl init or transport
// failure); a response with any status otherwise.
std::optional<ig_rest::HttpResponse> attempt(
    const ig_rest::Auth& auth, const std::string& path,
    const std::string& method, const std::string& jsonBody,
    const ig_rest::Headers& extraHeaders) {
    ensureCurlInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        backtest_log::error("IGRestClient: curl_easy_init failed");
        return std::nullopt;
    }

    // C# concatenates auth.url + path directly (the stored base URL ends
    // without a slash and every path starts with one) — same contract here.
    const std::string url = auth.url + path;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
                                "Content-Type: application/json; charset=UTF-8");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers,
                                ("X-IG-API-KEY: " + auth.apiKey).c_str());
    headers = curl_slist_append(headers, ("CST: " + auth.cst).c_str());
    headers = curl_slist_append(
        headers, ("X-SECURITY-TOKEN: " + auth.xSecurityToken).c_str());
    // A caller-supplied Version wins outright — appending the versionFor
    // default as well would put TWO Version headers on the wire.
    if (!ig_rest::hasHeader(extraHeaders, "Version")) {
        headers = curl_slist_append(
            headers, ("Version: " + ig_rest::versionFor(extraHeaders)).c_str());
    }
    for (const auto& [name, value] : extraHeaders) {
        headers = curl_slist_append(headers, (name + ": " + value).c_str());
    }

    ig_rest::HttpResponse response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    // Only attach a body when there is one: the confirms GET has none, and
    // POSTFIELDS on a bodiless GET would ship a POST-flavoured request.
    if (!jsonBody.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(jsonBody.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    // Bounded I/O on worker threads, same rationale as ElasticPublisher:
    // NOSIGNAL because resolver-timeout signals are not thread-safe; 30s
    // total matches the C# HttpClient timeout.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        backtest_log::error(std::string("IGRestClient: ") + method + " " + path
                            + " failed: " + curl_easy_strerror(rc));
        return std::nullopt;
    }
    return response;
}

}  // namespace

namespace ig_rest {

std::string versionFor(const Headers& extraHeaders) {
    for (const auto& [name, value] : extraHeaders) {
        if (caseInsensitiveEquals(name, "_method")
            && caseInsensitiveEquals(value, "delete")) {
            return "1";
        }
    }
    return "2";
}

bool hasHeader(const Headers& headers, const std::string_view name) {
    for (const auto& [headerName, value] : headers) {
        if (caseInsensitiveEquals(headerName, name)) {
            return true;
        }
    }
    return false;
}

std::optional<HttpResponse> execute(const Auth& auth, const std::string& path,
                                    const std::string& method,
                                    const std::string& jsonBody,
                                    const Headers& extraHeaders,
                                    const int maxRetries) {
    std::optional<HttpResponse> response;
    for (int attemptNo = 0; attemptNo <= maxRetries; ++attemptNo) {
        if (attemptNo > 0) {
            // 2^attempt seconds — the C# Polly WaitAndRetry schedule (2s, 4s).
            const auto backoff = std::chrono::seconds{1LL << attemptNo};
            backtest_log::error("IGRestClient: retry " + std::to_string(attemptNo)
                                + " for " + method + " " + path + " after "
                                + std::to_string(backoff.count()) + "s");
            std::this_thread::sleep_for(backoff);
        }
        response = attempt(auth, path, method, jsonBody, extraHeaders);
        if (!response) {
            continue;  // transport failure — retry
        }
        const bool transient = response->status >= 500 || response->status == 408;
        if (!transient) {
            return response;  // success or a definitive 4xx — caller decides
        }
    }
    // Retries exhausted: hand back whatever the last attempt produced (a
    // transient-status response, or nullopt when it never reached HTTP).
    return response;
}

}  // namespace ig_rest
