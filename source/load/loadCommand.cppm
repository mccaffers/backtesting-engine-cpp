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
#include "run/reporting/outcomeIndices.hpp"    // weekly index names + aliases
#include "shared/utilities/env.hpp"
#include "shared/utilities/queueKeys.hpp"
#include "load/redisLoader.hpp"
#include "shared/tradingDefinitions/config/runConfiguration.hpp"  // RunConfiguration -> json
#include "shared/tradingDefinitions/strategyConfig.hpp"      // StrategyConfig -> json (makeStrategy result)

export module loadCommand;

import std;                    // replaces <print>, <ranges>, <string>, <vector>
import backtestLog;            // backtest_log::logLine — timestamped, flushed stdout
import parameterGenerator;     // sweep::ParameterGenerator, sweep::Combination
import randomStrategySweep;    // buildRandomStrategySweep
import ohlcBreakoutStrategySweep; // buildOhlcBreakoutStrategySweep
import fvgStrategySweep;       // buildFvgStrategySweep
import keltnerFadeStrategySweep; // buildKeltnerFadeStrategySweep
import sessionRangeBreakoutStrategySweep; // buildSessionRangeBreakoutStrategySweep
import squeezeBreakoutStrategySweep; // buildSqueezeBreakoutStrategySweep
import nyOpenRangeBreakoutStrategySweep; // buildNyOpenRangeBreakoutStrategySweep
import liquiditySweepReversalStrategySweep; // buildLiquiditySweepReversalStrategySweep
import rangeVelocityStrategySweep; // buildRangeVelocityStrategySweep
import runConfigurationBuilder; // makeRunConfiguration, resolveSymbolGroups
import makeStrategy;           // sweep::makeStrategy
import makeOhlcBreakoutStrategy; // sweep::makeOhlcBreakoutStrategy
import makeFvgStrategy;        // sweep::makeFvgStrategy
import makeKeltnerFadeStrategy; // sweep::makeKeltnerFadeStrategy
import makeSessionRangeBreakoutStrategy; // sweep::makeSessionRangeBreakoutStrategy
import makeSqueezeBreakoutStrategy; // sweep::makeSqueezeBreakoutStrategy
import makeNyOpenRangeBreakoutStrategy; // sweep::makeNyOpenRangeBreakoutStrategy
import makeLiquiditySweepReversalStrategy; // sweep::makeLiquiditySweepReversalStrategy
import makeRangeVelocityStrategy; // sweep::makeRangeVelocityStrategy
import symbolGroups;           // sweep::cleanSymbols

export namespace sweep {

// Maps a swept combination onto a concrete strategy's config (minting a fresh
// UUID) — implemented per sweep by makeStrategy / makeOhlcBreakoutStrategy.
using StrategyFactory =
    tradingDefinitions::StrategyConfig (*)(const Combination&);

// Builds the keyed payloads for grid indices [begin, end): each combination is
// decoded lazily (combinationAt), mapped through the factory, and keyed under
// queue_keys::strategyPayloadKey(runId, <factory-minted UUID>). Exported so
// tests can pin the chunk/key/payload contract without Redis.
std::vector<RedisLoader::KeyedPayload> buildStrategyChunk(
    const ParameterGenerator& generator,
    StrategyFactory strategyFactory,
    const std::string& runId,
    std::size_t begin,
    std::size_t end);

}  // namespace sweep

export class LoadCommand {
public:
    static int run(int argc, const char* argv[]);
};

namespace {

// Groups digits with commas (1,000,000 — UK/US convention) so large grid
// sizes are readable at a glance. Hand-rolled rather than {:L} because the
// global locale defaults to "C" (no grouping) and named locales like
// en_GB.UTF-8 are not guaranteed to exist on every host.
std::string withThousands(const std::size_t n) {
    std::string s = std::to_string(n);
    for (std::size_t pos = s.size(); pos > 3;) {
        pos -= 3;
        s.insert(pos, ",");
    }
    return s;
}

// Payloads per pipelined Redis request: bounds producer memory (the grid is
// never materialised) and keeps each request comfortably sized.
constexpr std::size_t kChunkSize = 1000;

// Progress heartbeat for large grids, in strategies. A multiple of kChunkSize
// so the boundary always lands on a chunk edge.
constexpr std::size_t kProgressEvery = 100'000;

// Feeds RedisLoader::loadKeyedPayloadStream one lazily-built chunk at a time,
// walking the grid indices [0, total) exactly once.
class SweepChunkSource final : public RedisLoader::ChunkSource {
public:
    SweepChunkSource(const sweep::ParameterGenerator& generator,
                     const sweep::StrategyFactory strategyFactory,
                     std::string runId,
                     const std::size_t total)
        : generator_(generator),
          strategyFactory_(strategyFactory),
          runId_(std::move(runId)),
          total_(total) {}

    std::vector<RedisLoader::KeyedPayload> next() override {
        const std::size_t begin = next_;
        const std::size_t end = std::min(begin + kChunkSize, total_);
        next_ = end;
        if (begin != 0 && begin % kProgressEvery == 0) {
            backtest_log::logLine("LoadCommand: queued {}/{} strategies...",
                                  withThousands(begin), withThousands(total_));
        }
        return sweep::buildStrategyChunk(generator_, strategyFactory_, runId_,
                                         begin, end);
    }

private:
    const sweep::ParameterGenerator& generator_;
    sweep::StrategyFactory strategyFactory_;
    std::string runId_;
    std::size_t total_;
    std::size_t next_ = 0;
};

// Create this batch's weekly outcome indices and atomically repoint their
// -current aliases, once per load invocation — alias admin belongs to the
// seed, not the workers, where a late-flushing old-week run could flip an
// alias backwards. Best-effort: on the first failure log and stop (each admin
// call already retried transient failures internally; six more bounded-retry
// stalls against a dead host help nobody). Runs still carry the batch label
// and Elasticsearch auto-creates a weekly index on its first _bulk write, so
// only the alias lags — the next successful load self-heals it. With
// $ELASTIC_ENABLED=0 every call is a successful no-op, keeping a Redis-only
// local load Elastic-free.
void prepareWeeklyOutcomeIndices(const std::string& batchLabel) {
    for (const std::string_view base : outcome_index::kWeeklyBases) {
        const std::string index = outcome_index::weeklyIndex(base, batchLabel);
        if (elastic::ensureIndexExists(index) != 0 ||
            elastic::repointAlias(outcome_index::currentAlias(base), index) != 0) {
            backtest_log::logLine(
                "LoadCommand: weekly index/alias admin failed at {} — skipping "
                "the rest; the next successful load repoints the aliases",
                index);
            return;
        }
    }
}

}  // namespace

std::vector<RedisLoader::KeyedPayload> sweep::buildStrategyChunk(
    const ParameterGenerator& generator,
    const StrategyFactory strategyFactory,
    const std::string& runId,
    const std::size_t begin,
    const std::size_t end) {
    std::vector<RedisLoader::KeyedPayload> chunk;
    chunk.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        const tradingDefinitions::StrategyConfig config =
            strategyFactory(generator.combinationAt(i));
        // Pin to JSON explicitly to trigger the implicit StrategyConfig
        // conversion for .dump().
        const nlohmann::json j = config;
        chunk.push_back({queue_keys::strategyPayloadKey(runId, config.UUID),
                         j.dump()});
    }
    return chunk;
}

int LoadCommand::run(const int argc, const char* argv[]) {

    // Select the sweep from the command line, e.g. `load random`. The name is
    // required — omitting it or passing an unknown name is a usage error, not
    // a crash, so report the valid choices and bail. Each sweep pairs its
    // parameter grid with the factory that maps a combination onto that
    // strategy's config, so both are chosen together — a new sweep is one
    // extra branch here plus a mention in the error.
    const std::string_view sweepName = argc > 2 ? argv[2] : "";
    sweep::ParameterGenerator generator;
    sweep::StrategyFactory strategyFactory = nullptr;
    if (sweepName == "random") {
        generator = sweep::buildRandomStrategySweep();
        strategyFactory = sweep::makeStrategy;
    } else if (sweepName == "ohlcBreakout") {
        generator = sweep::buildOhlcBreakoutStrategySweep();
        strategyFactory = sweep::makeOhlcBreakoutStrategy;
    } else if (sweepName == "fvg") {
        generator = sweep::buildFvgStrategySweep();
        strategyFactory = sweep::makeFvgStrategy;
    } else if (sweepName == "keltnerFade") {
        generator = sweep::buildKeltnerFadeStrategySweep();
        strategyFactory = sweep::makeKeltnerFadeStrategy;
    } else if (sweepName == "sessionRangeBreakout") {
        generator = sweep::buildSessionRangeBreakoutStrategySweep();
        strategyFactory = sweep::makeSessionRangeBreakoutStrategy;
    } else if (sweepName == "squeezeBreakout") {
        generator = sweep::buildSqueezeBreakoutStrategySweep();
        strategyFactory = sweep::makeSqueezeBreakoutStrategy;
    } else if (sweepName == "nyOpenRangeBreakout") {
        generator = sweep::buildNyOpenRangeBreakoutStrategySweep();
        strategyFactory = sweep::makeNyOpenRangeBreakoutStrategy;
    } else if (sweepName == "liquiditySweepReversal") {
        generator = sweep::buildLiquiditySweepReversalStrategySweep();
        strategyFactory = sweep::makeLiquiditySweepReversalStrategy;
    } else if (sweepName == "rangeVelocity") {
        generator = sweep::buildRangeVelocityStrategySweep();
        strategyFactory = sweep::makeRangeVelocityStrategy;
    } else {
        std::println(stderr,
                     "LoadCommand: unknown sweep generator '{}' (valid: random, "
                     "ohlcBreakout, fvg, keltnerFade, sessionRangeBreakout, "
                     "squeezeBreakout, nyOpenRangeBreakout, "
                     "liquiditySweepReversal, rangeVelocity)",
                     sweepName);
        return 1;
    }

    // Surface the full sweep size (the grid fanned out across every run) and
    // wait for confirmation BEFORE anything touches Redis. The count is just
    // the product of the range sizes, so an over-eager grid is caught while
    // backing out is still free — Redis storage is O(count), and once a run is
    // advertised workers start draining it immediately. EOF (closed stdin)
    // counts as a decline so a non-interactive invocation can't sail past the
    // prompt.
    const auto symbolGroups = sweep::resolveSymbolGroups(generator.symbolGroups());
    const auto combinationCount = generator.combinationCount();
    backtest_log::logLine("LoadCommand: '{}' sweep: {} combination(s) x {} symbol group(s) = {} backtest(s)",
                          sweepName, withThousands(combinationCount), symbolGroups.size(),
                          withThousands(combinationCount * symbolGroups.size()));
    
    std::print("Press Enter to queue them (Ctrl+C to abort)... ");
    std::fflush(stdout);
    if (std::string ack; !std::getline(std::cin, ack)) {
        std::println(stderr, "LoadCommand: aborted, no confirmation on stdin");
        return 1;
    }

    // Freeze this load's batch identity (weekly index label + seed wall
    // clock) before anything is queued, then prepare the weekly indices and
    // aliases. scripts/load.sh pins $BACKTEST_BATCH across its per-strategy
    // invocations so a load straddling the ISO-week boundary cannot split one
    // batch over two labels.
    const sweep::BatchStamp batch = sweep::currentBatchStamp();
    backtest_log::logLine("LoadCommand: batch {} (execution {})", batch.label,
                          batch.executionTs);
    prepareWeeklyOutcomeIndices(batch.label);

    const auto redisHost = env::getOr("REDIS_HOST", "127.0.0.1");

    // One loader = one persistent Redis connection shared by every run below
    // (strategy streams and run descriptors alike), instead of a fresh
    // resolve/connect per push.
    RedisLoader loader(redisHost, 6379);

    // Fan out: every resolved symbol group becomes its own run — the sweep's
    // own list when it set one, the full kSymbolGroups default otherwise. A
    // comma-separated entry like "EURUSD,AUDUSD" is one run over multiple
    // instruments; separate entries are independent runs (each with its own
    // RUN_ID, strategy list and run descriptor on the shared RUN queue).
    for (const auto& group : symbolGroups) {
        const std::string symbols = sweep::cleanSymbols(group);

        // One RUN_ID per run; each combination becomes its own payload key,
        // distinguished by its parameter values and freshly-minted UUID.
        const auto runId = boost::uuids::to_string(boost::uuids::random_generator()());

        backtest_log::logLine("LoadCommand: sweeping {} parameter combination(s) for RUN_ID={} symbols={}",
                              withThousands(combinationCount), runId, symbols);

        // Stream every strategy into Redis BEFORE the run descriptor: payload
        // keys first, then their names onto the run's list (RedisLoader keeps
        // that order per chunk). A worker that sees the run immediately drains
        // the name list and retires the run when empty, so the full set must
        // already be present the moment the run becomes visible.
        SweepChunkSource source(generator, strategyFactory, runId, combinationCount);
        if (const auto strategyStatus = loader.loadKeyedPayloadStream(
                queue_keys::strategyKey(runId), source, queue_keys::PAYLOAD_TTL_SECONDS);
            strategyStatus != 0)
        {
            return strategyStatus;
        }

        // Now advertise the run so workers can pick it up. runJson is pinned to
        // nlohmann::JSON (not auto) because makeRunConfiguration returns a
        // RunConfiguration and relies on the implicit conversion for .dump().
        const nlohmann::json runJson = sweep::makeRunConfiguration(runId, symbols, batch);
        if (const auto runStatus = loader.loadPayload(queue_keys::RUN, runJson.dump());
            runStatus != 0)
        {
            return runStatus;
        }
    }

    return 0;
}
