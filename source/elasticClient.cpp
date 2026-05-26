// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "elasticClient.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include <curl/curl.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

namespace {

void ensureCurlInit() {
    static const struct CurlGlobal {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
        ~CurlGlobal() { curl_global_cleanup(); }
    } guard;
    (void)guard;
}

std::string envOr(const char* name, std::string fallback) {
    const char* val = std::getenv(name);
    return val ? std::string{val} : std::move(fallback);
}

std::string generateUuid() {
    static thread_local boost::uuids::random_generator gen;
    return boost::uuids::to_string(gen());
}

}  // namespace

int ElasticClient::putTradingResults(const TradingResults& results) {
    ensureCurlInit();

    const std::string host = envOr("ELASTICSEARCH_URL", "http://localhost:9200");
    const std::string url = host + "/trading_results/_doc/" + generateUuid();
    const std::string body = nlohmann::json(results).dump();

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "ElasticClient: curl_easy_init failed" << std::endl;
        return 1;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

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
        std::cerr << "ElasticClient: PUT failed: " << curl_easy_strerror(rc)
                  << std::endl;
        return 2;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        std::cerr << "ElasticClient: HTTP " << httpStatus << " from " << url
                  << std::endl;
        return 3;
    }
    std::cout << "ElasticClient: PUT " << url << " (HTTP " << httpStatus << ")"
              << std::endl;
    return 0;
}
