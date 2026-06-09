// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "loadCommand.hpp"

#include <array>
#include <charconv>
#include <print>
#include <ranges>
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
#include "run_configuration.hpp"
#include "trading_definitions/strategy.hpp"

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
    generator.addList("STOP_DISTANCE_IN_PIPS", {1.0, 1.5, 10.0});
    generator.addList("LIMIT_DISTANCE_IN_PIPS", {1.0, 1.5, 10.0});
    return generator;
}

// The run-level descriptor: what tick data to pull from QuestDB, plus the risk
// limits every strategy in the sweep runs under. Shared by every strategy in
// this sweep and linked to them by RUN_ID.
RunConfiguration makeRunConfiguration(const std::string& runId) {
    using namespace boost::decimal::literals;
    return RunConfiguration{
        .RUN_ID = runId,
        .SYMBOLS = "EURUSD",
        .LAST_MONTHS = 6,
        .STARTING_BALANCE = trading_definitions::DEFAULT_STARTING_BALANCE,
        // Cut a run off once it has lost 5% of the account (fail fast);
        // set <= 0 to run without any loss cutoff.
        .MAX_LOSS_PERCENT = 5_DD,
        // Cap on simultaneously open positions per run (<= 0 = unlimited).
        // The per-symbol gate already limits to one trade per symbol, so this
        // only bites on multi-symbol runs.
        .MAX_OPEN_TRADES = 10,
        // Flip to false to silence liquidated runs from Elasticsearch once
        // sweeps scale up and loss-limit cutoffs are expected noise.
        .REPORT_FAILURES = true,
    };
}

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
    const auto runId = boost::uuids::to_string(boost::uuids::random_generator()());
    const auto redisHost = env::getOr("REDIS_HOST", "127.0.0.1");

    // Build random here
    const auto generator = buildRandomStrategySweep();

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
    const nlohmann::json runJson = makeRunConfiguration(runId);
    return RedisLoader::loadPayload(redisHost, 6379, queue_keys::RUN,
                                    runJson.dump());
}
