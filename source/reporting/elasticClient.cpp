// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "reporting/elasticClient.hpp"

#include <iostream>
#include <string>

#include <curl/curl.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include "backtestLog.hpp"
#include "env.hpp"

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
size_t discardResponse(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/) {
    return size * nmemb;
}

}  // namespace

int ElasticClient::putTradingResults(const TradingResults& results) {
    // Allow runs to opt out of result reporting entirely (e.g. local backtests
    // with no Elastic instance). On by default to preserve existing behaviour.
    if (env::getOr("ELASTIC_ENABLED", "1") == "0") {
        return 0;
    }

    ensureCurlInit();

    const std::string host = env::getOr("ELASTIC_HOST", "http://localhost:9200");
    const std::string user = env::getOr("ELASTIC_USER", "");
    const std::string password = env::getOr("ELASTIC_USER_PASSWORD", "");
    const std::string url = host + "/trading_results/_doc/" + generateUuid();
    const std::string body = nlohmann::json(results).dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        backtest_log::error("ElasticClient: curl_easy_init failed");
        return 1;
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
        backtest_log::error(std::string("ElasticClient: PUT failed: ")
                            + curl_easy_strerror(rc));
        return 2;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        backtest_log::error("ElasticClient: HTTP " + std::to_string(httpStatus)
                            + " from " + url);
        return 3;
    }
    // Per-strategy success line is skipped under concurrent backtests (quiet).
    if (!backtest_log::quiet) {
        std::cout << "ElasticClient: PUT " << url << " (HTTP " << httpStatus << ")"
                  << std::endl;
    }
    return 0;
}
