// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "loadCommand.hpp"

#include <print>
#include <ranges>
#include <string>
#include <vector>

#include <boost/decimal.hpp>
#include <boost/decimal/literals.hpp>
#include <nlohmann/json.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "decimalConvert.hpp"
#include "env.hpp"
#include "parameterSweep.hpp"
#include "queueKeys.hpp"
#include "randomStrategySweep.hpp"
#include "redisLoader.hpp"
#include "runConfigurationBuilder.hpp"
#include "trading_definitions/strategy.hpp"

namespace {

using trading_definitions::Strategy;

// Maps one point in the parameter grid onto a Strategy. Fields not being swept
// keep their fixed defaults; swept fields are pulled from `combo`.
Strategy makeStrategy(const sweep::Combination& combo) {
    using namespace boost::decimal::literals;
    using namespace trading_definitions;

    return Strategy{
        // Each parameter combination gets its own UUID so a single backtest
        // result is uniquely identifiable and traceable back to its inputs.
        .UUID = boost::uuids::to_string(boost::uuids::random_generator()()),
        .TRADING_VARIABLES = TradingVariables{
            .STRATEGY = "RandomStrategy",
            .STOP_DISTANCE_IN_PIPS =
                decimal_convert::toDecimal(combo.get("STOP_DISTANCE_IN_PIPS")),
            .LIMIT_DISTANCE_IN_PIPS =
                decimal_convert::toDecimal(combo.get("LIMIT_DISTANCE_IN_PIPS")),
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
    const auto runId = boost::uuids::to_string(boost::uuids::random_generator()());
    const auto redisHost = env::getOr("REDIS_HOST", "127.0.0.1");

    // Build random here
    const auto generator = sweep::buildRandomStrategySweep();

    const auto combinations = generator.generateAllCombinations();

    std::println("LoadCommand: sweeping {} parameter combination(s) for RUN_ID={}",
                 combinations.size(), runId);

    // Serialise every swept strategy first. combinations is a sized range, so
    // std::ranges::to reserves up front (no manual reserve needed). The json type
    // is pinned explicitly because makeStrategy returns a Strategy and relies on
    // the implicit conversion for .dump().
    const auto strategyPayloads =
        combinations | std::views::transform([](const sweep::Combination& combo) {
            const nlohmann::json j = makeStrategy(combo);
            return j.dump();
        }) | std::ranges::to<std::vector<std::string>>();

    // Push all strategies BEFORE the run descriptor. A worker that sees the run
    // immediately drains the strategy list and retires the run when empty, so the
    // full set must already be present the moment the run becomes visible.
    const auto strategyKey = queue_keys::strategyKey(runId);
    const auto strategyStatus = RedisLoader::loadPayloadBatch(
        redisHost, 6379, strategyKey, strategyPayloads);
    if (strategyStatus != 0) {
        return strategyStatus;
    }

    // Now advertise the run so workers can pick it up. runJson is pinned to
    // nlohmann::json (not auto) because makeRunConfiguration returns a
    // RunConfiguration and relies on the implicit conversion for .dump().
    const nlohmann::json runJson = sweep::makeRunConfiguration(runId);
    return RedisLoader::loadPayload(redisHost, 6379, queue_keys::RUN,
                                    runJson.dump());
}
