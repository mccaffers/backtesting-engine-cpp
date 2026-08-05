// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <nlohmann/json.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "run/reporting/elasticPublisher.hpp"  // ensureIndexExists, repointAlias
#include "run/reporting/outcomeIndices.hpp"    // kExperimentsBase + weekly names
#include "shared/utilities/env.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "load/redisLoader.hpp"
#include "shared/experiments/experimentConfig.hpp"
#include "shared/experiments/experimentRunConfiguration.hpp"

export module experimentsCommand;

import std;
import backtestLog;             // backtest_log::logLine
export import dipRecoverySweep; // sweep::ExperimentSweepSpec/ExperimentFactory,
                                // buildDipRecoverySweep (re-exported so tests
                                // reach the whole seam via one import)
import runConfigurationBuilder; // sweep::resolveSymbolGroups, currentBatchStamp
import symbolGroups;            // sweep::cleanSymbols

export namespace sweep {

// Builds the keyed payloads for grid indices [begin, end): each combination
// is decoded lazily (combinationAt), mapped through the factory, and keyed
// under queue_keys::experimentPayloadKey(runId, <factory-minted UUID>) — the
// exact parallel of buildStrategyChunk. Exported so tests can pin the
// chunk/key/payload contract without Redis.
std::vector<RedisLoader::KeyedPayload> buildExperimentChunk(
    const ParameterGenerator& generator,
    ExperimentFactory experimentFactory,
    const std::string& runId,
    std::size_t begin,
    std::size_t end);

}  // namespace sweep

export class ExperimentsCommand {
public:
    static int run(int argc, const char* argv[]);
};

namespace {

// Same digit grouping as LoadCommand (see loadCommand.cppm for why this is
// hand-rolled rather than {:L}).
std::string withThousands(const std::size_t n) {
    std::string s = std::to_string(n);
    for (std::size_t pos = s.size(); pos > 3;) {
        pos -= 3;
        s.insert(pos, ",");
    }
    return s;
}

// Payloads per pipelined Redis request / progress heartbeat — LoadCommand's
// values, for the same bounded-memory reasons.
constexpr std::size_t kChunkSize = 1000;
constexpr std::size_t kProgressEvery = 100'000;

// Feeds RedisLoader::loadKeyedPayloadStream one lazily-built chunk at a time.
// A copy of SweepChunkSource with the factory type swapped — deliberately not
// templated/shared: two small concrete classes read better than one generic
// seam, and the pair can drift independently.
class ExperimentChunkSource final : public RedisLoader::ChunkSource {
public:
    ExperimentChunkSource(const sweep::ParameterGenerator& generator,
                          const sweep::ExperimentFactory experimentFactory,
                          std::string runId,
                          const std::size_t total)
        : generator_(generator),
          experimentFactory_(experimentFactory),
          runId_(std::move(runId)),
          total_(total) {}

    std::vector<RedisLoader::KeyedPayload> next() override {
        const std::size_t begin = next_;
        const std::size_t end = std::min(begin + kChunkSize, total_);
        next_ = end;
        if (begin != 0 && begin % kProgressEvery == 0) {
            backtest_log::logLine("ExperimentsCommand: queued {}/{} experiments...",
                                  withThousands(begin), withThousands(total_));
        }
        return sweep::buildExperimentChunk(generator_, experimentFactory_,
                                           runId_, begin, end);
    }

private:
    const sweep::ParameterGenerator& generator_;
    sweep::ExperimentFactory experimentFactory_;
    std::string runId_;
    std::size_t total_;
    std::size_t next_ = 0;
};

// Create this batch's weekly experiments index and atomically repoint its
// -current alias — kExperimentsBase ONLY (deliberately not in kWeeklyBases:
// `load` must not create empty experiment indices, and `experiments` has no
// business touching the outcome bases). Best-effort, same doctrine as
// LoadCommand::prepareWeeklyOutcomeIndices; with $ELASTIC_ENABLED=0 every
// call is a successful no-op, keeping a Redis-only local run Elastic-free.
void prepareWeeklyExperimentsIndex(const std::string& batchLabel) {
    const std::string index =
        outcome_index::weeklyIndex(outcome_index::kExperimentsBase, batchLabel);
    if (elastic::ensureIndexExists(index) != 0 ||
        elastic::repointAlias(
            outcome_index::currentAlias(outcome_index::kExperimentsBase),
            index) != 0) {
        backtest_log::logLine(
            "ExperimentsCommand: weekly index/alias admin failed at {} — the "
            "next successful experiments load repoints the alias",
            index);
    }
}

}  // namespace

std::vector<RedisLoader::KeyedPayload> sweep::buildExperimentChunk(
    const ParameterGenerator& generator,
    const ExperimentFactory experimentFactory,
    const std::string& runId,
    const std::size_t begin,
    const std::size_t end) {
    std::vector<RedisLoader::KeyedPayload> chunk;
    chunk.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        const experiments::ExperimentConfig config =
            experimentFactory(generator.combinationAt(i));
        // Pin to JSON explicitly to trigger the implicit conversion for
        // .dump() — buildStrategyChunk's pattern.
        const nlohmann::json j = config;
        chunk.push_back({queue_keys::experimentPayloadKey(runId, config.UUID),
                         j.dump()});
    }
    return chunk;
}

int ExperimentsCommand::run(const int argc, const char* argv[]) {

    // Select the sweep from the command line, e.g. `experiments dipRecovery`.
    // The name is required — omitting it or passing an unknown name is a
    // usage error, not a crash, so report the valid choices and bail. A new
    // sweep is one extra branch here plus a mention in the error.
    const std::string_view sweepName = argc > 2 ? argv[2] : "";
    sweep::ExperimentSweepSpec spec;
    if (sweepName == "dipRecovery") {
        spec = sweep::buildDipRecoverySweep();
    } else {
        std::println(stderr,
                     "ExperimentsCommand: unknown experiment sweep '{}' "
                     "(valid: dipRecovery)",
                     sweepName);
        return 1;
    }

    // Surface the full sweep size and wait for confirmation BEFORE anything
    // touches Redis — LoadCommand's gate, for the same reasons. EOF (closed
    // stdin) counts as a decline so a non-interactive invocation can't sail
    // past the prompt.
    const auto symbolGroups =
        sweep::resolveSymbolGroups(spec.generator.symbolGroups());
    const auto combinationCount = spec.generator.combinationCount();
    backtest_log::logLine(
        "ExperimentsCommand: '{}' sweep: {} experiment(s) x {} symbol group(s) = {} evaluation(s)",
        sweepName, withThousands(combinationCount), symbolGroups.size(),
        withThousands(combinationCount * symbolGroups.size()));

    std::print("Press Enter to queue them (Ctrl+C to abort)... ");
    std::fflush(stdout);
    if (std::string ack; !std::getline(std::cin, ack)) {
        std::println(stderr, "ExperimentsCommand: aborted, no confirmation on stdin");
        return 1;
    }

    // Freeze this load's batch identity before anything is queued, then
    // prepare the weekly experiments index + alias (see LoadCommand for the
    // seed-not-workers doctrine).
    const sweep::BatchStamp batch = sweep::currentBatchStamp();
    backtest_log::logLine("ExperimentsCommand: batch {} (execution {})",
                          batch.label, batch.executionTs);
    prepareWeeklyExperimentsIndex(batch.label);

    const auto redisHost = env::getOr("REDIS_HOST", "127.0.0.1");

    // One loader = one persistent Redis connection shared by every run below.
    RedisLoader loader(redisHost, 6379);

    // Fan out: every resolved symbol group becomes its own run (its own
    // RUN_ID, experiment list and run descriptor on the EXPERIMENT_RUN
    // queue). A comma-separated entry is one run over multiple instruments.
    for (const auto& group : symbolGroups) {
        const std::string symbols = sweep::cleanSymbols(group);

        const auto runId =
            boost::uuids::to_string(boost::uuids::random_generator()());

        backtest_log::logLine(
            "ExperimentsCommand: sweeping {} experiment(s) for RUN_ID={} symbols={}",
            withThousands(combinationCount), runId, symbols);

        // Stream every experiment into Redis BEFORE the run descriptor
        // (payloads first, then names — RedisLoader keeps that order per
        // chunk), so the full set is present the moment the run is visible.
        ExperimentChunkSource source(spec.generator, spec.factory, runId,
                                     combinationCount);
        if (const auto payloadStatus = loader.loadKeyedPayloadStream(
                queue_keys::experimentKey(runId), source,
                queue_keys::PAYLOAD_TTL_SECONDS);
            payloadStatus != 0)
        {
            return payloadStatus;
        }

        // Now advertise the run so analysis workers can pick it up. The
        // descriptor carries the sweep's own tick window (per-sweep
        // LAST_MONTHS/OFFSET_MONTHS — see ExperimentSweepSpec).
        const nlohmann::json runJson = experiments::ExperimentRunConfiguration{
            .RUN_ID = runId,
            .SYMBOLS = symbols,
            .BATCH = batch.label,
            .EXECUTION_TS = batch.executionTs,
            .LAST_MONTHS = spec.lastMonths,
            .OFFSET_MONTHS = spec.offsetMonths,
        };
        if (const auto runStatus =
                loader.loadPayload(queue_keys::EXPERIMENT_RUN, runJson.dump());
            runStatus != 0)
        {
            return runStatus;
        }
    }

    return 0;
}
