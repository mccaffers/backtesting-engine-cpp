// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"
#include "run/reporting/outcomeIndices.hpp"
#include "analysis/reporting/experimentResults.hpp"

export module experimentElastic;

import std;

// Typed front-end over the shared Elasticsearch publisher for experiment
// results — a small PARALLEL client, deliberately not ElasticClient, which is
// welded to tradingDefinitions::Configuration and the results/winners index
// routing. Serialises on the calling thread, hands the document to the
// publisher's background flusher (elastic::enqueueDocument) and returns
// immediately — a pool worker finishing an evaluation never blocks on
// Elastic.
export class ExperimentElastic {
public:
    static void putExperimentResults(const ExperimentResults& results);
};

void ExperimentElastic::putExperimentResults(const ExperimentResults& r) {
    // Deterministic _id: RUN_ID is already per symbol group and each
    // experiment produces exactly one aggregate doc per group, so
    // RUN_ID:experiment-UUID identifies it stably (no :symbol suffix) and
    // the flusher's bulk retries become idempotent overwrites.
    elastic::enqueueDocument(
        outcome_index::weeklyIndex(outcome_index::kExperimentsBase,
                                   r.runConfig.BATCH),
        nlohmann::json(r).dump(),
        r.RUN_ID + ":" + r.experiment.UUID);
}
