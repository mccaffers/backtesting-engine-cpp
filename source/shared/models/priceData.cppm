// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

export module priceData;

import std;  // replaces <chrono>, <cstdint>, <string>

// ask/bid are scaled fixed-point integers as stored in QuestDB: the real price
// multiplied by the symbol's fixed multiplier (FX majors x100000, JPY pairs &
// metals x1000, indices/commodities x100 — see the symbolScale module). e.g.
// EURUSD 1.10001 is stored as 110001. Integer prices keep the per-tick loop free
// of software-emulated decimal arithmetic; PnL/balance stay decimal64_t at the
// boundary. Largest scaled value (~4.5M for indices) fits comfortably in int32.
export struct PriceData {
    std::int32_t ask;
    std::int32_t bid;
    std::chrono::system_clock::time_point timestamp;
    std::string symbol;

    PriceData(std::int32_t ask, std::int32_t bid,
              const std::chrono::system_clock::time_point& ts, const std::string& symbol)
        : ask(ask), bid(bid), timestamp(ts), symbol(symbol) {}

    PriceData() : ask(0), bid(0), timestamp{}, symbol("") {}
};
