// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include <stdio.h>
#include <string>
#include <vector>

namespace strategy {

struct OHLCVariables {
    int OHLC_COUNT;
    int OHLC_MINUTES;
};

struct OHLCRSIVariables {
    int RSI_LONG;
    int RSI_SHORT;
};

struct TradingVariables {
    std::string STRATEGY;
    double STOP_DISTANCE_IN_PIPS;
    double LIMIT_DISTANCE_IN_PIPS;
    double TRADING_SIZE;
};

struct Strategy {
    std::string UUID;
    TradingVariables TRADING_VARIABLES;
    std::vector<OHLCVariables> OHLC_VARIABLES;
    OHLCRSIVariables OHLC_RSI_VARIABLES;
};

struct Configuration {
    std::string RUN_ID;
    std::string SYMBOLS;
    int LAST_MONTHS;
    Strategy STRATEGY;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OHLCVariables,
    OHLC_COUNT,
    OHLC_MINUTES
);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OHLCRSIVariables,
    RSI_LONG,
    RSI_SHORT
);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TradingVariables,
    STRATEGY,
    STOP_DISTANCE_IN_PIPS,
    LIMIT_DISTANCE_IN_PIPS,
    TRADING_SIZE
);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Strategy,
    UUID,
    TRADING_VARIABLES,
    OHLC_VARIABLES,
    OHLC_RSI_VARIABLES
);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Configuration,
    RUN_ID,
    SYMBOLS,
    LAST_MONTHS,
    STRATEGY
)
} // namespace ns
