// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

class FxMacroDataClient {
public:
    using QueryParams = std::vector<std::pair<std::string, std::string>>;
    using Transport = std::function<std::string(const std::string& method,
                                                const std::string& url,
                                                const std::string& body)>;

    explicit FxMacroDataClient(
        std::string apiKey = {},
        std::string baseUrl = "https://api.fxmacrodata.com/v1",
        Transport transport = {});

    [[nodiscard]] nlohmann::json dataCatalogue(const std::string& currency,
                                               QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json announcements(const std::string& currency,
                                               const std::string& indicator,
                                               QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json latestAnnouncements(const std::string& currency,
                                                     QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json announcementChanges(QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json calendar(const std::string& currency,
                                          QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json predictions(const std::string& currency,
                                             const std::string& indicator,
                                             QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json forex(const std::string& base,
                                       const std::string& quote,
                                       QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json cot(const std::string& currency,
                                     QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json commodity(const std::string& indicator,
                                           QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json commoditiesLatest(QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json curves(const std::string& currency,
                                        QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json curveProxies(const std::string& currency,
                                              QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json forwardCurves(const std::string& currency,
                                               QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json rateDifferentials(const std::string& base,
                                                   const std::string& quote,
                                                   QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json forwardDifferentials(const std::string& base,
                                                      const std::string& quote,
                                                      QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json marketSessions(QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json riskSentiment(QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json news(const std::string& currency,
                                      QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json pressReleases(const std::string& currency,
                                               QueryParams params = {}) const;
    [[nodiscard]] nlohmann::json graphql(const std::string& query,
                                         nlohmann::json variables = nlohmann::json::object()) const;
    [[nodiscard]] nlohmann::json request(const std::string& path,
                                         QueryParams params = {},
                                         std::string method = "GET",
                                         nlohmann::json body = nlohmann::json::object()) const;

    [[nodiscard]] std::string buildUrl(const std::string& path, QueryParams params = {}) const;

private:
    std::string apiKey_;
    std::string baseUrl_;
    Transport transport_;

    [[nodiscard]] nlohmann::json requestJson(const std::string& method,
                                             const std::string& path,
                                             QueryParams params,
                                             const nlohmann::json& body = nlohmann::json::object()) const;
};
