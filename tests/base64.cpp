// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "shared/utilities/base64.hpp"

TEST_CASE("Base64 RFC 4648 test vectors", "[base64]") {
    CHECK(Base64::b64encode("") == "");
    CHECK(Base64::b64decode("") == "");

    CHECK(Base64::b64encode("f") == "Zg==");
    CHECK(Base64::b64decode("Zg==") == "f");

    CHECK(Base64::b64encode("fo") == "Zm8=");
    CHECK(Base64::b64decode("Zm8=") == "fo");

    CHECK(Base64::b64encode("foo") == "Zm9v");
    CHECK(Base64::b64decode("Zm9v") == "foo");

    CHECK(Base64::b64encode("foob") == "Zm9vYg==");
    CHECK(Base64::b64decode("Zm9vYg==") == "foob");

    CHECK(Base64::b64encode("fooba") == "Zm9vYmE=");
    CHECK(Base64::b64decode("Zm9vYmE=") == "fooba");

    CHECK(Base64::b64encode("foobar") == "Zm9vYmFy");
    CHECK(Base64::b64decode("Zm9vYmFy") == "foobar");
}

TEST_CASE("Base64 binary data round-trips correctly", "[base64]") {
    std::string allBytes;
    allBytes.reserve(256);
    for (int i = 0; i < 256; ++i) {
        allBytes.push_back(static_cast<char>(i));
    }

    const std::string encoded = Base64::b64encode(allBytes);
    const std::string decoded = Base64::b64decode(encoded);
    CHECK(decoded == allBytes);
}

TEST_CASE("Base64 URL-safe characters decode identically", "[base64]") {
    // '+' -> '-' (index 62) and '/' -> '_' (index 63)
    // "????" with binary values 62, 63, 62, 63:
    // Standard: "+/+/"
    // URL-safe: "-_-_"
    const std::string standard = "+/+/";
    const std::string urlSafe = "-_-_";
    CHECK(Base64::b64decode(standard) == Base64::b64decode(urlSafe));
}

TEST_CASE("Base64 rejects strings whose length is not a multiple of 4", "[base64]") {
    CHECK_THROWS_AS(Base64::b64decode("a"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("ab"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("abc"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("abcde"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("abcdef"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("abcdefg"), std::invalid_argument);
}

TEST_CASE("Base64 rejects invalid characters", "[base64]") {
    CHECK_THROWS_AS(Base64::b64decode("Zm9 v"), std::invalid_argument);  // space
    CHECK_THROWS_AS(Base64::b64decode("Zm9\nv"), std::invalid_argument); // newline
    CHECK_THROWS_AS(Base64::b64decode("Zm@v"), std::invalid_argument);   // '@'
    CHECK_THROWS_AS(Base64::b64decode("Zm#v"), std::invalid_argument);   // '#'
    CHECK_THROWS_AS(Base64::b64decode("Zm$v"), std::invalid_argument);   // '$'
    CHECK_THROWS_AS(Base64::b64decode("Zm%v"), std::invalid_argument);   // '%'
}

TEST_CASE("Base64 rejects invalid padding configurations", "[base64]") {
    CHECK_THROWS_AS(Base64::b64decode("===="), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("A==="), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("AA=A"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("A=AA"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("=AAA"), std::invalid_argument);
    CHECK_THROWS_AS(Base64::b64decode("=AA="), std::invalid_argument);
    // Mid-stream padding
    CHECK_THROWS_AS(Base64::b64decode("Zm==Zm9v"), std::invalid_argument);
}
