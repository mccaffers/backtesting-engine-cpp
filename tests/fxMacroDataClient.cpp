#include <catch2/catch_test_macros.hpp>

#include <string>

#include "shared/utilities/fxMacroDataClient.hpp"

TEST_CASE("FxMacroDataClient builds authenticated macro endpoint requests", "[fxmacrodata]") {
    std::string method;
    std::string url;
    std::string body;

    FxMacroDataClient client{
        "test-key",
        "https://api.fxmacrodata.com/v1/",
        [&](const std::string& m, const std::string& u, const std::string& b) {
            method = m;
            url = u;
            body = b;
            return R"({"ok":true})";
        }};

    const auto result = client.predictions("USD", "non_farm_payrolls", {{"limit", "1"}});
    CHECK(result.at("ok").get<bool>());
    CHECK(method == "GET");
    CHECK(url == "https://api.fxmacrodata.com/v1/predictions/usd/non_farm_payrolls?limit=1&api_key=test-key");

    client.rateDifferentials("EUR", "USD", {{"tenor", "2y"}});
    CHECK(url == "https://api.fxmacrodata.com/v1/rate_differentials/eur/usd?tenor=2y&api_key=test-key");

    client.graphql("query { marketSessions { name } }");
    CHECK(method == "POST");
    CHECK(url == "https://api.fxmacrodata.com/v1/graphql?api_key=test-key");
    CHECK(body.find("marketSessions") != std::string::npos);
}
