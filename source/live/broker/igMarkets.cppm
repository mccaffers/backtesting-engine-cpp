// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// igMarkets — the IG broker wire shapes and the order seams, mirroring the
// C# engine's TradeOpenObj / TradeCloseObj / IGPositionResponseObject /
// MarketElement models. Everything here is either a serialisation target
// (field spelling matches the IG REST API on purpose) or a pure codec over
// one — the HTTP/gating machinery lives in igRequests, the flow logic in
// orderChannel.
//
// IG's OTC dealing model, for reference: POST /positions/otc opens; closing
// is the SAME endpoint with an extra "_method: DELETE" header (Version 1).
// Both answer with { dealReference } only — the dealId exists once the deal
// confirms, and the external position producer reconciles it into the PL#
// book from the broker's own position list.

module;

#include <nlohmann/json.hpp>

export module igMarkets;

import std;  // replaces <cstdint>, <functional>, <optional>, <string>, <vector>

export namespace ig {

// POST /positions/otc request body — the C# TradeOpenObj. Distances are in
// pips; a 0 distance means that leg is disarmed and encodeTradeOpen omits
// the field (IG rejects a literal zero distance). size is decimal at the
// broker (mini contracts), so the engine's integer TRADING_SIZE arrives
// here already multiplied by the market's sizeModifier.
struct TradeOpenObj {
    std::string currencyCode;
    std::string epic;
    std::string expiry = "-";
    std::string direction;  // "BUY" | "SELL"
    double size{};
    bool forceOpen = true;
    bool guaranteedStop = false;
    std::string orderType = "MARKET";
    std::int32_t stopDistance{};   // pips; 0 = no stop leg
    std::int32_t limitDistance{};  // pips; 0 = no limit leg
    // Client-generated idempotency token ([A-Za-z0-9_-], max 30 chars); IG
    // echoes it in the response and the confirm stream so fills can be
    // matched to requests. Omitted from the JSON when empty.
    std::string dealReference;
};

// POST /positions/otc + "_method: DELETE" request body — the C# TradeCloseObj.
// direction is the CLOSING direction (opposite of the open position's).
struct TradeCloseObj {
    std::string orderType = "MARKET";
    std::string direction;  // "BUY" | "SELL"
    std::string dealId;
    double size{};
};

// The open/close response body — the C# IGPositionResponseObject. errorCode
// is null on success.
struct IGPositionResponseObject {
    std::string dealReference;
    std::optional<std::string> errorCode;
};

// GET /confirms/{dealReference} response (Version 1) — the deal's fate.
// dealStatus is definitive once present: "ACCEPTED" or "REJECTED"; anything
// else (or a 404 while the confirm propagates) reads as still pending.
struct DealConfirmation {
    std::string dealId;
    std::string dealReference;
    std::string dealStatus;  // "ACCEPTED" | "REJECTED" | pending/unknown
    std::string reason;      // rejection reason; "SUCCESS" on accepts
};

// GET /markets market payload — the C# MarketElement. Parsed by the REST
// client when the positions read path lands; decimals arrive as double at
// this boundary.
struct MarketElement {
    std::string instrumentName;
    std::string expiry;
    std::string epic;
    std::string instrumentType;
    double lotSize{};
    double high{};
    double low{};
    double percentageChange{};
    double netChange{};
    double bid{};
    double offer{};
    std::string updateTime;
    int delayTime{};
    bool streamingPricesAvailable{};
    std::string marketStatus;
    int scalingFactor{};
};

// One deal inside GET /positions — the C# PositionElement. Nullable broker
// fields stay optional: absent in the JSON means absent here.
struct PositionElement {
    double contractSize{};
    std::string createdDate;
    std::string dealId;
    double dealSize{};
    std::string dealReference;
    std::string direction;  // "BUY" | "SELL"
    std::optional<double> limitLevel;
    double openLevel{};
    std::string currency;
    bool controlledRisk{};
    std::optional<double> stopLevel;
    std::optional<bool> trailingStep;
    std::optional<double> trailingStopDistance;
    std::optional<double> limitedRiskPremium;
};

// GET /positions pairs each deal with its market snapshot.
struct Position {
    std::optional<PositionElement> position;
    std::optional<MarketElement> market;
};

struct AccountPositions {
    std::vector<Position> positions;
};

// ---- codecs (pure, unit-tested without a broker) ----

// The request bodies, field names exactly as IG (and the C# serializer)
// spell them.
std::string encodeTradeOpen(const TradeOpenObj& order);
std::string encodeTradeClose(const TradeCloseObj& close);

// Parse an open/close response body. nullopt when the body is not a JSON
// object (HTML error pages, truncation) — the C# "Failed to parse response"
// branch. A missing dealReference parses as empty (the caller decides what
// that means).
std::optional<IGPositionResponseObject> parsePositionResponse(
    const std::string& body);

// Parse a confirms body. nullopt for non-objects; missing fields parse as
// empty (the caller treats an empty dealStatus as still pending).
std::optional<DealConfirmation> parseDealConfirmation(const std::string& body);

// Closing a position trades the OPPOSITE side: BUY position -> SELL order.
std::string closingDirection(std::string_view openBrokerDirection);

// ---- order seams ----

// Everything about the originating decision the broker call layer needs
// beyond the wire payload: the request-gate duplicate key is
// strategyUuid + openDirection (C# $"{strategyId}{reqObj.direction}" — note
// the OPEN direction even on a close request, so a close within the open's
// 30s suppression window is refused, exactly as in C#), and the live-trades
// audit trail wants symbol/strategy.
struct OrderContext {
    std::string strategyUuid;
    std::string strategyName;
    std::string symbol;
    std::string openDirection;  // broker vocabulary: "BUY" | "SELL"
};

// Outcome of one placement attempt, split the way the order channel reacts:
//   Accepted — IG took the request; dealReference tracks the deal until the
//              confirm/producer supplies the dealId.
//   Rejected — a definitive broker NO: safe to release the trade lock and
//              re-enter early. (The HTTP path currently maps nothing here —
//              a non-2xx could mean the order half-exists, so it fails
//              conservatively; the confirms endpoint will populate this.)
//   Failed   — no definitive answer (transport error, non-2xx, unparseable
//              body): the C# "trade didn't complete" path; the lock is
//              extended because the order MAY still be live at the broker.
enum class OpenStatus { Accepted, Rejected, Failed };

struct OpenResult {
    OpenStatus status{OpenStatus::Failed};
    std::string dealReference;  // Accepted only (IG's echo, not ours)
    // Broker deal id from the confirms poll; EMPTY when the confirm never
    // resolved (the deal is booked anyway and cannot be strategy-closed
    // until an id appears — the close path's blank-dealId guard).
    std::string dealId;
    std::string reason;         // Rejected / Failed diagnostics
};

// Outcome of one close attempt:
//   Ok     — IG accepted the close request.
//   Gone   — the request layer produced no response at all; the C# engine
//            treats this as "missing from IG" and deletes the book entry so
//            a phantom position cannot haunt the strategy logic forever.
//   Failed — IG answered but not OK (or unparseably): the position still
//            exists as far as anyone knows; keep it on the book.
enum class CloseStatus { Ok, Gone, Failed };

struct CloseResult {
    CloseStatus status{CloseStatus::Failed};
    std::string reason;
};

using PlaceOrder =
    std::function<OpenResult(const TradeOpenObj&, const OrderContext&)>;
using PlaceClose =
    std::function<CloseResult(const TradeCloseObj&, const OrderContext&)>;

}  // namespace ig

namespace ig {

std::string encodeTradeOpen(const TradeOpenObj& order) {
    nlohmann::json body{
        {"currencyCode", order.currencyCode},
        {"epic", order.epic},
        {"expiry", order.expiry},
        {"direction", order.direction},
        {"size", order.size},
        {"forceOpen", order.forceOpen},
        {"guaranteedStop", order.guaranteedStop},
        {"orderType", order.orderType},
    };
    if (order.stopDistance > 0) {
        body["stopDistance"] = order.stopDistance;
    }
    if (order.limitDistance > 0) {
        body["limitDistance"] = order.limitDistance;
    }
    if (!order.dealReference.empty()) {
        body["dealReference"] = order.dealReference;
    }
    return body.dump();
}

std::string encodeTradeClose(const TradeCloseObj& close) {
    const nlohmann::json body{
        {"orderType", close.orderType},
        {"direction", close.direction},
        {"dealId", close.dealId},
        {"size", close.size},
    };
    return body.dump();
}

std::optional<IGPositionResponseObject> parsePositionResponse(
    const std::string& body) {
    const nlohmann::json parsed =
        nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    IGPositionResponseObject response;
    if (const auto it = parsed.find("dealReference");
        it != parsed.end() && it->is_string()) {
        response.dealReference = it->get<std::string>();
    }
    if (const auto it = parsed.find("errorCode");
        it != parsed.end() && it->is_string()) {
        response.errorCode = it->get<std::string>();
    }
    return response;
}

std::optional<DealConfirmation> parseDealConfirmation(const std::string& body) {
    const nlohmann::json parsed =
        nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) {
        return std::nullopt;
    }
    DealConfirmation confirmation;
    const auto readString = [&parsed](const char* key, std::string& out) {
        if (const auto it = parsed.find(key);
            it != parsed.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    readString("dealId", confirmation.dealId);
    readString("dealReference", confirmation.dealReference);
    readString("dealStatus", confirmation.dealStatus);
    readString("reason", confirmation.reason);
    return confirmation;
}

std::string closingDirection(const std::string_view openBrokerDirection) {
    return openBrokerDirection == "BUY" ? "SELL" : "BUY";
}

}  // namespace ig
