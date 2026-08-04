// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// marketDefinitions — engine symbol -> broker market mapping, the C#
// engine's MarketDescriptions ported verbatim from its PRODUCTION config
// (every epic below is what the C# engine trades today — including the
// quirks: XAUUSD is denominated in GBP on this account, indices and
// commodities mostly settle in GBP, and GBRIDXGBP's mini epic differs from
// its CFD epic).
//
// Two fields drive live trading, everything else is model fidelity:
//   epicMini          (IGMarketIdentiferMini) — the market actually traded;
//                     the order channel always deals MINI contracts
//   tradeSizeModifier (TradeSizeModifier)     — multiplies the strategy's
//                     TRADING_SIZE; 0 encodes the C# null = no scaling
//
// The C# model's `strategies` list is deliberately absent: strategy
// selection here comes from the winners system (liveWinners), not from
// per-market config. A symbol MISSING from this table means "do not trade
// it live" (the order channel logs and drops the signal), so the table
// doubles as the live trading allowlist.
//
// Same table discipline as symbolScale: constexpr, sorted, binary-searched,
// compile-time-checked — and the two tables cover the SAME 29 symbols
// (pinned by test), so anything priced can be traded and vice versa.

export module marketDefinitions;

import std;  // replaces <array>, <functional>, <string_view>

export namespace live {

// The C# MarketType values in use.
enum class MarketType { Forex, Indice };

// The C# MarketDescriptions (field spellings modernised; the originals are
// noted where they differ).
struct MarketDefinition {
    std::string_view symbol;
    std::string_view igMarketId;         // IGMarketID (== symbol throughout)
    std::string_view epicCfd;            // IGMarketIdentifer — full CFD contract
    std::string_view epicMini;           // IGMarketIdentiferMini — WHAT WE TRADE
    std::string_view currency;
    std::string_view polygonIdentifier;  // PolygonIdentifer; "" = none
    double polygonScale;                 // PolygonScale; 0 = none
    MarketType type;
    double tradeSizeModifier;            // TradeSizeModifier; 0 = C# null

    // The C# call-site contract: `if (TradeSizeModifier is not null)
    // reqObj.size *= modifier` — absent means unscaled.
    [[nodiscard]] constexpr double sizeModifier() const noexcept {
        return tradeSizeModifier > 0.0 ? tradeSizeModifier : 1.0;
    }
};

// MUST stay sorted ascending by symbol — enforced below, binary search
// depends on it. Columns: {symbol, IGMarketID, CFD epic, MINI epic,
// currency, polygon id, polygon scale, type, size modifier}.
inline constexpr std::array<MarketDefinition, 29> kMarkets{{
    {"AUDNZD", "AUDNZD", "CS.D.AUDNZD.CFD.IP", "CS.D.AUDNZD.MINI.IP", "NZD",
     "C.AUD/NZD", 0, MarketType::Forex, 0},
    {"AUDUSD", "AUDUSD", "CS.D.AUDUSD.CFD.IP", "CS.D.AUDUSD.MINI.IP", "USD",
     "C.AUD/USD", 0, MarketType::Forex, 0},
    {"AUSIDXAUD", "AUSIDXAUD", "IX.D.ASX.IFS.IP", "IX.D.ASX.IFS.IP", "GBP",
     "", 0, MarketType::Indice, 0},
    {"BRENTCMDUSD", "BRENTCMDUSD", "CC.D.LCO.UMP.IP", "CC.D.LCO.UMP.IP",
     "GBP", "", 0, MarketType::Indice, 0},
    {"COPPERCMDUSD", "COPPERCMDUSD", "CC.D.HG.UMP.IP", "CC.D.HG.UMP.IP",
     "GBP", "", 0, MarketType::Indice, 0},
    {"DEUIDXEUR", "DEUIDXEUR", "IX.D.DAX.IFS.IP", "IX.D.DAX.IFS.IP", "GBP",
     "", 0, MarketType::Indice, 0},
    {"EURAUD", "EURAUD", "CS.D.EURAUD.CFD.IP", "CS.D.EURAUD.MINI.IP", "AUD",
     "C.EUR/AUD", 0, MarketType::Forex, 0},
    {"EURCHF", "EURCHF", "CS.D.EURCHF.CFD.IP", "CS.D.EURCHF.MINI.IP", "CHF",
     "C.EUR/CHF", 0, MarketType::Forex, 0},
    {"EURGBP", "EURGBP", "CS.D.EURGBP.CFD.IP", "CS.D.EURGBP.MINI.IP", "GBP",
     "C.EUR/GBP", 0, MarketType::Forex, 0},
    {"EURJPY", "EURJPY", "CS.D.EURJPY.CFD.IP", "CS.D.EURJPY.MINI.IP", "JPY",
     "C.EUR/JPY", 0, MarketType::Forex, 0},
    {"EURNOK", "EURNOK", "CS.D.EURNOK.CFD.IP", "CS.D.EURNOK.MINI.IP", "NOK",
     "C.EUR/NOK", 0, MarketType::Forex, 0},
    {"EURUSD", "EURUSD", "CS.D.EURUSD.CFD.IP", "CS.D.EURUSD.MINI.IP", "USD",
     "C.EUR/USD", 0, MarketType::Forex, 0},
    {"FRAIDXEUR", "FRAIDXEUR", "IX.D.CAC.IFS.IP", "IX.D.CAC.IFS.IP", "GBP",
     "", 0, MarketType::Indice, 0},
    {"GBPJPY", "GBPJPY", "CS.D.GBPJPY.CFD.IP", "CS.D.GBPJPY.MINI.IP", "JPY",
     "C.GBP/JPY", 0, MarketType::Forex, 0},
    {"GBPUSD", "GBPUSD", "CS.D.GBPUSD.CFD.IP", "CS.D.GBPUSD.MINI.IP", "USD",
     "C.GBP/USD", 0, MarketType::Forex, 0},
    // The one market whose mini contract is a different epic family from
    // its CFD — trading the CFD epic by mistake would 2x the exposure the
    // 0.5 modifier is there to halve.
    {"GBRIDXGBP", "GBRIDXGBP", "IX.D.FTSE.CFD.IP", "IX.D.FTSE.IFM.IP", "GBP",
     "", 0, MarketType::Indice, 0.5},
    {"HKGIDXHKD", "HKGIDXHKD", "IX.D.HANGSENG.IFU.IP", "IX.D.HANGSENG.IFU.IP",
     "USD", "", 0, MarketType::Indice, 0},
    {"JPNIDXJPY", "JPNIDXJPY", "IX.D.NIKKEI.IFM.IP", "IX.D.NIKKEI.IFM.IP",
     "USD", "", 0, MarketType::Indice, 0},
    {"LIGHTCMDUSD", "LIGHTCMDUSD", "CC.D.CL.UMP.IP", "CC.D.CL.UMP.IP", "GBP",
     "", 0, MarketType::Indice, 0},
    {"NZDUSD", "NZDUSD", "CS.D.NZDUSD.CFD.IP", "CS.D.NZDUSD.MINI.IP", "USD",
     "C.NZD/USD", 0, MarketType::Forex, 0},
    {"USA30IDXUSD", "USA30IDXUSD", "IX.D.DOW.IFS.IP", "IX.D.DOW.IFS.IP",
     "GBP", "", 0, MarketType::Indice, 0},
    {"USA500IDXUSD", "USA500IDXUSD", "IX.D.SPTRD.IFS.IP", "IX.D.SPTRD.IFS.IP",
     "GBP", "", 0, MarketType::Indice, 0},
    {"USATECHIDXUSD", "USATECHIDXUSD", "IX.D.NASDAQ.IFS.IP",
     "IX.D.NASDAQ.IFS.IP", "GBP", "", 0, MarketType::Indice, 0},
    {"USDCAD", "USDCAD", "CS.D.USDCAD.CFD.IP", "CS.D.USDCAD.MINI.IP", "CAD",
     "C.USD/CAD", 0, MarketType::Forex, 0},
    {"USDCHF", "USDCHF", "CS.D.USDCHF.CFD.IP", "CS.D.USDCHF.MINI.IP", "CHF",
     "C.USD/CHF", 0, MarketType::Forex, 0},
    {"USDJPY", "USDJPY", "CS.D.USDJPY.CFD.IP", "CS.D.USDJPY.MINI.IP", "JPY",
     "C.USD/JPY", 0, MarketType::Forex, 0},
    {"USDSEK", "USDSEK", "CS.D.USDSEK.CFD.IP", "CS.D.USDSEK.MINI.IP", "SEK",
     "C.USD/SEK", 0, MarketType::Forex, 0},
    {"XAGUSD", "XAGUSD", "CS.D.CFDSILVER.CFM.IP", "CS.D.CFDSILVER.CFM.IP",
     "USD", "C.XAG/USD", 100, MarketType::Forex, 0.2},
    // XAUUSD modifier raised from the C# 0.5: IG rejects size 0.5 on this
    // epic as below the market minimum (every 2026-07-10 XAUUSD open was
    // REJECTED), so TRADING_SIZE 1 must reach the broker as 1.0.
    {"XAUUSD", "XAUUSD", "CS.D.CFPGOLD.CFP.IP", "CS.D.CFPGOLD.CFP.IP", "GBP",
     "C.XAU/USD", 0, MarketType::Forex, 1.0},
}};

static_assert([] {
    for (std::size_t i = 1; i < kMarkets.size(); ++i) {
        if (!(kMarkets[i - 1].symbol < kMarkets[i].symbol)) return false;
    }
    return true;
}(), "live::kMarkets must be sorted ascending by symbol — binary search depends on it");

// The symbols live trading may book, in table (= sorted) order. The winners
// fetch fans out one query per (strategy, symbol) over exactly this list — a
// winner on any other symbol could only book a worker whose orders the
// channel drops, so the allowlist doubles as the fetch universe.
inline constexpr auto kTradableSymbols = [] {
    std::array<std::string_view, kMarkets.size()> symbols{};
    for (std::size_t i = 0; i < kMarkets.size(); ++i) {
        symbols[i] = kMarkets[i].symbol;
    }
    return symbols;
}();

// The matching definition, or nullptr when the symbol is not tradable live.
// Cold path (per order, not per tick) but the sorted table makes it cheap
// anyway.
[[nodiscard]] constexpr const MarketDefinition* findMarket(
    std::string_view symbol) noexcept {
    std::size_t lo = 0;
    std::size_t hi = kMarkets.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1);
        const auto& entry = kMarkets[mid];
        if (entry.symbol < symbol) {
            lo = mid + 1;
        } else if (symbol < entry.symbol) {
            hi = mid;
        } else {
            return &entry;
        }
    }
    return nullptr;
}

// Reverse lookup for the position producer: the market whose MINI epic (the
// contract the order channel trades, and what IG's /positions book reports
// back) matches. The table is sorted by symbol, not epic, so this is a linear
// scan — 29 entries, once a minute. nullptr = an epic the engine does not
// trade (the producer skips it, like the C# SavePositions match).
[[nodiscard]] constexpr const MarketDefinition* findMarketByEpicMini(
    std::string_view epic) noexcept {
    for (const auto& entry : kMarkets) {
        if (entry.epicMini == epic) {
            return &entry;
        }
    }
    return nullptr;
}

// Lookup seam for the order channel: injectable so unit tests can supply
// their own definitions (and a missing symbol) without depending on what the
// real table currently lists — test the machinery, not the table.
using MarketLookup = std::function<const MarketDefinition*(std::string_view)>;

}  // namespace live
