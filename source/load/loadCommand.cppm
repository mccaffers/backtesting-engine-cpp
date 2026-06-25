// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module; 

#include <nlohmann/json.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "shared/utilities/env.hpp"
#include "shared/utilities/parameterSweep.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "shared/redis/producer/redisLoader.hpp"
#include "shared/tradingDefinitions/config/runConfiguration.hpp"  // RunConfiguration -> json
#include "shared/tradingDefinitions/strategy.hpp"           // Strategy -> json (makeStrategy result)

export module loadCommand;

import std;                    // replaces <print>, <ranges>, <string>, <vector>
import randomStrategySweep;    // buildRandomStrategySweep
import runConfigurationBuilder; // makeRunConfiguration
import makeStrategy;           // sweep::makeStrategy

export class LoadCommand {
public:
    static int run(int argc, const char* argv[]);
};

int LoadCommand::run(const int argc, const char* argv[]) {

    // One RUN_ID identifies the whole sweep; each combination becomes its own
    // queue entry, distinguished by its parameter values.
    const auto runId = boost::uuids::to_string(boost::uuids::random_generator()());

    // Select the sweep generator from the command line, e.g. `load random`.
    // Defaults to "random" — the only generator today — when omitted. An
    // unknown name is a usage error, not a crash, so report it and bail.
    const std::string_view sweepName = argc > 2 ? argv[2] : "random";
    sweep::ParameterGenerator generator;
    try {
        generator = sweep::buildSweep(sweepName);
    } catch (const std::exception& ex) {
        std::println(stderr, "LoadCommand: {}", ex.what());
        return 1;
    }

    const auto combinations = generator.generateAllCombinations();

    std::println("LoadCommand: sweeping {} parameter combination(s) for RUN_ID={}", combinations.size(), runId);

    // Serialise every swept strategy first. combinations is a sized range, so
    // std::ranges::to reserves up front (no manual reserve needed). The json type
    // is pinned explicitly because makeStrategy returns a Strategy and relies on
    // the implicit conversion for .dump().
    const auto strategyPayloads = combinations 
        | std::views::transform([](const sweep::Combination& combo) {
            // Pin to JSON explicitly to trigger implicit Strategy conversion
            const nlohmann::json j = sweep::makeStrategy(combo);
            return j.dump();
        }) 
        | std::ranges::to<std::vector>();

    // Push all strategies BEFORE the run descriptor. A worker that sees the run
    // immediately drains the strategy list and retires the run when empty, so the
    // full set must already be present the moment the run becomes visible.
    const auto strategyKey = queue_keys::strategyKey(runId);

    const auto redisHost = env::getOr("REDIS_HOST", "127.0.0.1");
    if (const auto strategyStatus = RedisLoader::loadPayloadBatch(redisHost, 6379, strategyKey, strategyPayloads);
        strategyStatus != 0)
    {
        return strategyStatus;
    }

    // Now advertise the run so workers can pick it up. runJson is pinned to
    // nlohmann::JSON (not auto) because makeRunConfiguration returns a
    // RunConfiguration and relies on the implicit conversion for .dump().
    const nlohmann::json runJson = sweep::makeRunConfiguration(runId);
    return RedisLoader::loadPayload(redisHost, 6379, queue_keys::RUN, runJson.dump());
}
