// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/utilities/fxMacroDataClient.hpp"

#include "shared/utilities/env.hpp"

#include <curl/curl.h>

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string ensureLeadingSlash(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path;
    }
    return "/" + path;
}

std::string lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string encode(std::string value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string performHttp(const std::string& method,
                        const std::string& url,
                        const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "backtesting-engine-cpp-fxmacrodata/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    if (method == "POST") {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("FXMacroData request failed: ") + curl_easy_strerror(rc));
    }
    if (status < 200 || status >= 300) {
        throw std::runtime_error("FXMacroData request returned HTTP " + std::to_string(status));
    }
    return response;
}

}  // namespace

FxMacroDataClient::FxMacroDataClient(std::string apiKey,
                                     std::string baseUrl,
                                     Transport transport)
    : apiKey_(apiKey.empty()
                  ? env::getOr("FXMACRODATA_API_KEY", env::getOr("FXMD_API_KEY", ""))
                  : std::move(apiKey)),
      baseUrl_(trimTrailingSlash(std::move(baseUrl))),
      transport_(std::move(transport)) {}

std::string FxMacroDataClient::buildUrl(const std::string& path, QueryParams params) const {
    if (!apiKey_.empty()) {
        params.emplace_back("api_key", apiKey_);
    }

    std::ostringstream url;
    url << baseUrl_ << ensureLeadingSlash(path);
    char separator = '?';
    for (const auto& [key, value] : params) {
        url << separator << encode(key) << '=' << encode(value);
        separator = '&';
    }
    return url.str();
}

nlohmann::json FxMacroDataClient::requestJson(const std::string& method,
                                              const std::string& path,
                                              QueryParams params,
                                              const nlohmann::json& body) const {
    const std::string bodyText = method == "POST" ? body.dump() : std::string{};
    const std::string text = transport_
        ? transport_(method, buildUrl(path, std::move(params)), bodyText)
        : performHttp(method, buildUrl(path, std::move(params)), bodyText);
    return nlohmann::json::parse(text);
}

nlohmann::json FxMacroDataClient::dataCatalogue(const std::string& currency,
                                                QueryParams params) const {
    return requestJson("GET", "/data_catalogue/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::announcements(const std::string& currency,
                                                const std::string& indicator,
                                                QueryParams params) const {
    return requestJson("GET", "/announcements/" + encode(lower(currency)) + "/" + encode(indicator), std::move(params));
}

nlohmann::json FxMacroDataClient::latestAnnouncements(const std::string& currency,
                                                      QueryParams params) const {
    return requestJson("GET", "/announcements/" + encode(lower(currency)) + "/latest", std::move(params));
}

nlohmann::json FxMacroDataClient::announcementChanges(QueryParams params) const {
    return requestJson("GET", "/announcements/changes", std::move(params));
}

nlohmann::json FxMacroDataClient::calendar(const std::string& currency,
                                           QueryParams params) const {
    return requestJson("GET", "/calendar/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::predictions(const std::string& currency,
                                              const std::string& indicator,
                                              QueryParams params) const {
    return requestJson("GET", "/predictions/" + encode(lower(currency)) + "/" + encode(indicator), std::move(params));
}

nlohmann::json FxMacroDataClient::forex(const std::string& base,
                                        const std::string& quote,
                                        QueryParams params) const {
    return requestJson("GET", "/forex/" + encode(lower(base)) + "/" + encode(lower(quote)), std::move(params));
}

nlohmann::json FxMacroDataClient::cot(const std::string& currency,
                                      QueryParams params) const {
    return requestJson("GET", "/cot/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::commodity(const std::string& indicator,
                                            QueryParams params) const {
    return requestJson("GET", "/commodities/" + encode(indicator), std::move(params));
}

nlohmann::json FxMacroDataClient::commoditiesLatest(QueryParams params) const {
    return requestJson("GET", "/commodities/latest", std::move(params));
}

nlohmann::json FxMacroDataClient::curves(const std::string& currency,
                                         QueryParams params) const {
    return requestJson("GET", "/curves/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::curveProxies(const std::string& currency,
                                               QueryParams params) const {
    return requestJson("GET", "/curve_proxies/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::forwardCurves(const std::string& currency,
                                                QueryParams params) const {
    return requestJson("GET", "/forward_curves/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::rateDifferentials(const std::string& base,
                                                    const std::string& quote,
                                                    QueryParams params) const {
    return requestJson("GET", "/rate_differentials/" + encode(lower(base)) + "/" + encode(lower(quote)), std::move(params));
}

nlohmann::json FxMacroDataClient::forwardDifferentials(const std::string& base,
                                                       const std::string& quote,
                                                       QueryParams params) const {
    return requestJson("GET", "/forward_differentials/" + encode(lower(base)) + "/" + encode(lower(quote)), std::move(params));
}

nlohmann::json FxMacroDataClient::marketSessions(QueryParams params) const {
    return requestJson("GET", "/market_sessions", std::move(params));
}

nlohmann::json FxMacroDataClient::riskSentiment(QueryParams params) const {
    return requestJson("GET", "/risk_sentiment", std::move(params));
}

nlohmann::json FxMacroDataClient::news(const std::string& currency,
                                       QueryParams params) const {
    return requestJson("GET", "/news/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::pressReleases(const std::string& currency,
                                                QueryParams params) const {
    return requestJson("GET", "/press-releases/" + encode(lower(currency)), std::move(params));
}

nlohmann::json FxMacroDataClient::graphql(const std::string& query,
                                          nlohmann::json variables) const {
    return requestJson("POST", "/graphql", {}, {{"query", query}, {"variables", std::move(variables)}});
}

nlohmann::json FxMacroDataClient::request(const std::string& path,
                                          QueryParams params,
                                          std::string method,
                                          nlohmann::json body) const {
    return requestJson(std::move(method), path, std::move(params), body);
}
