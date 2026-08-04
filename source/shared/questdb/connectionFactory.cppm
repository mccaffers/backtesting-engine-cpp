// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// Single place that turns the process environment into a pg-wire
// DatabaseConnection: $QUESTDB_HOST (default 127.0.0.1), $QUESTDB_PORT
// (default 8812), and the stock QuestDB credentials (qdb/admin/quest).
// Callers that receive the host some other way (the run command takes it via
// argv) pass it as `hostOverride`; the port still comes from the environment
// so a non-default instance can be targeted without a rebuild.

module;

#include "shared/utilities/env.hpp"

export module connectionFactory;

import std;                 // replaces <charconv>, <format>, <stdexcept>, <string>, <string_view>
import databaseConnection;  // DatabaseConnection

export namespace questdb {

DatabaseConnection connectionFromEnv(std::string_view hostOverride = {});

}  // namespace questdb

namespace questdb {

DatabaseConnection connectionFromEnv(std::string_view hostOverride) {
    const std::string host = hostOverride.empty()
                                 ? env::getOr("QUESTDB_HOST", "127.0.0.1")
                                 : std::string(hostOverride);

    // from_chars over the full string rather than std::stoi: a misconfigured
    // $QUESTDB_PORT fails with the offending value in the message instead of
    // stoi's bare "invalid_argument", and trailing junk ("8812x") is rejected
    // rather than silently truncated.
    const std::string portText = env::getOr("QUESTDB_PORT", "8812");
    int port = 0;
    const auto [ptr, ec] =
        std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (ec != std::errc{} || ptr != portText.data() + portText.size()) {
        throw std::runtime_error(
            std::format("QUESTDB_PORT is not a valid port: '{}'", portText));
    }

    return DatabaseConnection(host, port, "qdb", "admin", "quest");
}

}  // namespace questdb
