//
//  StrategyConfiguration.cpp
//  source
//
//  Created by Ryan McCaffery on 15/01/2025.
//

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
} // namespace ns
