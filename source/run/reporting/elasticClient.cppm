// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

module;

#include <nlohmann/json.hpp>

#include "run/reporting/elasticPublisher.hpp"
#include "run/reporting/outcomeIndices.hpp"
#include "run/reporting/tradeDocument.hpp"
#include "run/reporting/tradingResults.hpp"

export module elasticClient;

import std;            // replaces <chrono>, <string>, <vector>
import rollingWindow;  // rolling::kFullHistory — routes {9,0} to the winners index
import trade;          // Trade, Direction
import symbolScale;    // symbol_scale::getPriceScale

// Typed front-end over the shared Elasticsearch publisher. Index names come
// from outcomeIndices.hpp: each outcome index is the batch's weekly one
// ("backtesting-results-2026-28" — config.BATCH rides in from the load, so a
// run's documents target the same index no matter when the flusher delivers
// them), and gate-cleared full-history runs are split off into the winners
// index, the small population live's winner selection boots against. The
// transport, env config and auth all live in the shared publisher so the run
// path and the shared redis-consumer path report through one implementation.
//
// Every method serialises on the calling thread, hands the document(s) to the
// publisher's background flusher (elastic::enqueueDocument) and returns
// immediately — a pool worker finishing a run never blocks on Elastic, and the
// flusher's periodic _bulk batches replace the per-run PUT storm that fast
// sweeps were answering with 429s. Delivery failures are logged and
// dead-lettered from the flusher thread; there is nothing to report back here.
export class ElasticClient {
public:
    static void putTradingResults(const TradingResults& results);
    static void putTradingFailure(const TradingFailure& failure);
    // Compact per-run terminal record; lands in the weekly kFinalBase index.
    static void putTradeFinal(const TradeFinal& result);
    // One document per closed trade, queued for outcome_index::kTradesIndex.
    // Doc _id = RUN_ID:STRATEGY.UUID:tradeId (RUN_ID:tradeId when the config
    // carries no UUID) so bulk retries overwrite rather than duplicate — the
    // single-doc paths' random-UUID fallback would break that idempotency.
    // Strategy metadata is taken from `config` (Trade.strategyId/strategyName
    // are never populated by the engine).
    static void bulkPutTrades(const std::vector<Trade>& closedTrades,
                              const tradingDefinitions::Configuration& config,
                              const std::string& hostname);
};

// Which base index putTradingResults routes a completed run to: the terminal
// full-history window — exactly live's eligibility bar, so the two stay in
// lockstep through rolling::kFullHistory if the ladder ever grows — lands in
// the winners index; every other window is a screening pass and stays in the
// general results index. Exported so a test can pin the split.
export std::string_view resultsBaseFor(const int lastMonths,
                                       const int offsetMonths) {
    const bool fullHistory = lastMonths == rolling::kFullHistory.lastMonths &&
                             offsetMonths == rolling::kFullHistory.offsetMonths;
    return fullHistory ? outcome_index::kWinnersBase
                       : outcome_index::kResultsBase;
}

namespace {

// Deterministic Elasticsearch _id: one strategy execution produces at most one
// document per index, so RUN_ID + strategy UUID identifies it stably — the
// publisher's retries become idempotent overwrites instead of duplicates, and
// the same execution can be correlated across the three indices. Falls back to
// a publisher-generated UUID for configs the sweep did not stamp with one.
std::string outcomeDocId(const std::string& runId,
                         const tradingDefinitions::Configuration& config) {
    const std::string& uuid = config.STRATEGY.UUID;
    return uuid.empty() ? std::string{} : runId + ":" + uuid;
}

}  // namespace

void ElasticClient::putTradingResults(const TradingResults& results) {
    elastic::enqueueDocument(
        outcome_index::weeklyIndex(resultsBaseFor(results.config.LAST_MONTHS,
                                                  results.config.OFFSET_MONTHS),
                                   results.config.BATCH),
        nlohmann::json(results).dump(),
        outcomeDocId(results.RUN_ID, results.config));
}

void ElasticClient::putTradingFailure(const TradingFailure& failure) {
    elastic::enqueueDocument(
        outcome_index::weeklyIndex(outcome_index::kFailuresBase,
                                   failure.config.BATCH),
        nlohmann::json(failure).dump(),
        outcomeDocId(failure.RUN_ID, failure.config));
}

void ElasticClient::putTradeFinal(const TradeFinal& result) {
    elastic::enqueueDocument(
        outcome_index::weeklyIndex(outcome_index::kFinalBase,
                                   result.config.BATCH),
        nlohmann::json(result).dump(),
        outcomeDocId(result.RUN_ID, result.config));
}

void ElasticClient::bulkPutTrades(const std::vector<Trade>& closedTrades,
                                  const tradingDefinitions::Configuration& config,
                                  const std::string& hostname) {
    if (closedTrades.empty()) {
        return;
    }

    // Per-trade _id extends the per-run outcome id with the trade's own id
    // ("T{n}", unique within one strategy execution).
    const std::string outcomeId = outcomeDocId(config.RUN_ID, config);
    const std::string idPrefix =
        (outcomeId.empty() ? config.RUN_ID : outcomeId) + ":";

    // One wall-clock stamp for the whole run's trades: they are queued
    // together, and a shared value groups them in ops queries.
    const std::string ingestedAt = elastic::nowIsoUtc();
    const std::string& strategyName = config.STRATEGY.TRADING_VARIABLES.STRATEGY;

    std::vector<elastic::BulkDoc> docs;
    docs.reserve(closedTrades.size());
    for (const Trade& trade : closedTrades) {
        // Convert stored INT32 points back to real decimal prices for Kibana.
        // Unknown symbols (scale 0) keep the raw integers; priceScale = 0 in
        // the doc tells the consumer which form it is looking at.
        const int priceScale = symbol_scale::getPriceScale(trade.symbol);
        const auto toPrice = [priceScale](std::int32_t points) {
            return priceScale > 0 ? static_cast<double>(points) / priceScale
                                  : static_cast<double>(points);
        };
        // Same normalisation as the "Trade Closed" log line: PnL is stored in
        // points × size, so divide by both points-per-pip and size to get pips
        // of price movement. pnlPoints carries the lossless integer alongside.
        const double pnlPips =
            (trade.scalingFactor != 0 && trade.size != 0)
                ? static_cast<double>(trade.pnl) /
                      (static_cast<double>(trade.scalingFactor) * trade.size)
                : 0.0;
        const std::string closeIso = TradeDocument::isoUtcMillis(trade.closeTime);
        const TradeDocument doc{
            .RUN_ID = config.RUN_ID,
            .timestamp = closeIso,  // @timestamp = sim close time
            .ingestedAt = ingestedAt,
            .hostname = hostname,
            .strategyUuid = config.STRATEGY.UUID,
            .strategyName = strategyName,
            .tradeId = trade.id,
            .symbol = trade.symbol,
            .direction =
                trade.direction == Direction::LONG ? "LONG" : "SHORT",
            .size = trade.size,
            .entryPrice = toPrice(trade.entryPrice),
            .entryBid = toPrice(trade.entryBid),
            .entryAsk = toPrice(trade.entryAsk),
            .closePrice = toPrice(trade.closePrice),
            .stopPrice = toPrice(trade.stopPrice),
            .limitPrice = toPrice(trade.limitPrice),
            .stopDistancePips = trade.stopDistancePips,
            .limitDistancePips = trade.limitDistancePips,
            .openTime = TradeDocument::isoUtcMillis(trade.openTime),
            .closeTime = closeIso,
            .holdSeconds = std::chrono::duration<double>(trade.closeTime -
                                                         trade.openTime)
                               .count(),
            .pnlPoints = trade.pnl,
            .pnlPips = pnlPips,
            .liquidated = trade.liquidated,
            .scalingFactor = trade.scalingFactor,
            .priceScale = priceScale,
        };
        docs.push_back({idPrefix + trade.id, nlohmann::json(doc).dump()});
    }
    elastic::enqueueDocuments(std::string{outcome_index::kTradesIndex},
                              std::move(docs));
}
