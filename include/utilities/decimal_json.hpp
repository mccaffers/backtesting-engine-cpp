// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <stdexcept>
#include <string>
#include <system_error>
#include <boost/decimal.hpp>
#include <boost/decimal/charconv.hpp>
#include <nlohmann/json.hpp>

// Bridge boost::decimal::decimal64_t into nlohmann::json. Without this,
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE on structs containing decimal64_t fields
// fails to compile because the library can't find a (de)serializer for the type.
//
// Values move through JSON as strings (e.g. "0.0001") and are parsed with
// boost::decimal::from_chars. Going via a string skips the binary-float
// detour, so a literal like 0.1 in the source JSON survives the round-trip
// without snapping to the nearest IEEE-754 double.
//
// nlohmann's recommended way to add support for a third-party type is to
// specialize adl_serializer rather than injecting free functions into someone
// else's namespace. The C# analogue would be writing a custom JsonConverter<T>
// and registering it on the serializer options.
namespace nlohmann {
template <>
struct adl_serializer<boost::decimal::decimal64_t> {
    static void to_json(json& j, const boost::decimal::decimal64_t& value) {
        // 64 chars is comfortably above the longest possible decimal64_t
        // representation (16 significant digits + sign + point + exponent).
        char buffer[64];
        const auto result = boost::decimal::to_chars(buffer, buffer + sizeof(buffer), value);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("decimal_json: to_chars failed serializing decimal64_t");
        }
        j = std::string(buffer, result.ptr);
    }

    static void from_json(const json& j, boost::decimal::decimal64_t& value) {
        const auto& str = j.get_ref<const std::string&>();
        const auto result = boost::decimal::from_chars(str.data(), str.data() + str.size(), value);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("decimal_json: from_chars failed parsing '" + str + "'");
        }
    }
};
}
