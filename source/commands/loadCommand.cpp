// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "loadCommand.hpp"

#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <boost/decimal.hpp>
#include <boost/decimal/charconv.hpp>
#include <boost/decimal/literals.hpp>
#include <nlohmann/json.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "env.hpp"
#include "parameterSweep.hpp"
#include "queueKeys.hpp"
#include "redisLoader.hpp"
#include "trading_definitions.hpp"

namespace {

using trading_definitions::RunConfiguration;
using trading_definitions::Strategy;

// Converts a swept double into a decimal64_t via its shortest round-trip decimal
// string. Routing through text (rather than constructing from the binary double)
// stops clean decimals like 1.5 / 2.0 from snapping to an IEEE-754 neighbour,
// consistent with how decimal_json.hpp moves values through JSON. Sweep with
// binary-exact steps (halves, quarters) or explicit lists to keep this exact.
boost::decimal::decimal64_t toDecimal(double value) {
    std::array<char, 64> buffer;
    const auto [ptr, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::runtime_error("loadCommand: to_chars failed for swept decimal");
    }
    boost::decimal::decimal64_t result;
    const auto parsed = boost::decimal::from_chars(buffer.data(), ptr, result);
    if (parsed.ec != std::errc{}) {
        throw std::runtime_error("loadCommand: from_chars failed for swept decimal");
    }
    return result;
}

// Declares which parameters to sweep for the RandomStrategy. Keeping the ranges
// in one place means a new strategy (or extra swept parameter) is a localised
// edit: register it here, then read it back in makeConfiguration().
sweep::ParameterGenerator buildRandomStrategySweep() {
    sweep::ParameterGenerator generator;
    // generator.addRange("OHLC_COUNT", 80, 20, 140);  // 80, 100, 120, 140
    // generator.addList("OHLC_MINUTES", {1, 3, 5, 8});
    generator.addList("STOP_DISTANCE_IN_PIPS", {1.0, 1.5, 2.0});
    generator.addList("LIMIT_DISTANCE_IN_PIPS", {1.0, 1.5, 2.0});
    return generator;
}

// The run-level descriptor: what tick data to pull from QuestDB. Shared by every
// strategy in this sweep and linked to them by RUN_ID.
RunConfiguration makeRunConfiguration(const std::string& runId) {
    return RunConfiguration{
        .RUN_ID = runId,
        .SYMBOLS = "EURUSD,AUDUSD",
        .LAST_MONTHS = 2,
    };
}

// Maps one point in the parameter grid onto a Strategy. Fields not being swept
// keep their fixed defaults; swept fields are pulled from `combo`.
Strategy makeStrategy(const sweep::Combination& combo) {
    using namespace boost::decimal::literals;
    using namespace trading_definitions;

    return Strategy{
        .UUID = "",
        .TRADING_VARIABLES = TradingVariables{
            .STRATEGY = "RandomStrategy",
            .STOP_DISTANCE_IN_PIPS = toDecimal(combo.get("STOP_DISTANCE_IN_PIPS")),
            .LIMIT_DISTANCE_IN_PIPS = toDecimal(combo.get("LIMIT_DISTANCE_IN_PIPS")),
            .TRADING_SIZE = 1_DD,
        },
        .OHLC_VARIABLES = {
            OHLCVariables{
                // Only read OHLC params when the sweep actually registers them;
                // they default to 0 otherwise (see buildRandomStrategySweep).
                .OHLC_COUNT = combo.has("OHLC_COUNT") ? combo.getInt("OHLC_COUNT") : 0,
                .OHLC_MINUTES = combo.has("OHLC_MINUTES") ? combo.getInt("OHLC_MINUTES") : 0,
            },
        },
        .STRATEGY_VARIABLES = StrategyVariables{
            .OHLC_RSI_VARIABLES = OHLCRSIVariables{.RSI_LONG = 60, .RSI_SHORT = 40},
        },
    };
}

}  // namespace

int LoadCommand::run() {
    // One RUN_ID identifies the whole sweep; each combination becomes its own
    // queue entry, distinguished by its parameter values.
    const std::string runId = boost::uuids::to_string(boost::uuids::random_generator()());
    const std::string redisHost = env::getOr("REDIS_HOST", "127.0.0.1");

    // Build random here
    const sweep::ParameterGenerator generator = buildRandomStrategySweep();

    const std::vector<sweep::Combination> combinations = generator.generateAllCombinations();

    std::cout << "LoadCommand: sweeping " << combinations.size()
              << " parameter combination(s) for RUN_ID=" << runId << std::endl;

    // Serialise every swept strategy first.
    std::vector<std::string> strategyPayloads;
    strategyPayloads.reserve(combinations.size());
    for (const sweep::Combination& combo : combinations) {
        const nlohmann::json j = makeStrategy(combo);
        strategyPayloads.push_back(j.dump());
    }

    // Push all strategies BEFORE the run descriptor. A worker that sees the run
    // immediately drains the strategy list and retires the run when empty, so the
    // full set must already be present the moment the run becomes visible.
    const std::string strategyKey = queue_keys::strategyKey(runId);
    const int strategyStatus = RedisLoader::loadPayloadBatch(
        redisHost, 6379, strategyKey, strategyPayloads);
    if (strategyStatus != 0) {
        return strategyStatus;
    }

    // Now advertise the run so workers can pick it up.
    const nlohmann::json runJson = makeRunConfiguration(runId);
    return RedisLoader::loadPayload(redisHost, 6379, queue_keys::RUN,
                                    runJson.dump());
}
