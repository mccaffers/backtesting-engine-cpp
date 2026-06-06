// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "loadCommand.hpp"

#include <boost/decimal/literals.hpp>
#include <nlohmann/json.hpp>

#include "env.hpp"
#include "redisLoader.hpp"
#include "trading_definitions.hpp"

int LoadCommand::run() {
    using namespace boost::decimal::literals;
    using namespace trading_definitions;

    const Configuration config{
        .RUN_ID = "UNIQUE_IDENTIFIER",
        .SYMBOLS = "EURUSD,AUDUSD",
        .LAST_MONTHS = 2,
        .STRATEGY = Strategy{
            .UUID = "",
            .TRADING_VARIABLES = TradingVariables{
                .STRATEGY = "RandomStrategy",
                .STOP_DISTANCE_IN_PIPS = 1.5_DD,
                .LIMIT_DISTANCE_IN_PIPS = 1.5_DD,
                .TRADING_SIZE = 1_DD,
            },
            .OHLC_VARIABLES = {
                OHLCVariables{.OHLC_COUNT = 60, .OHLC_MINUTES = 100},
            },
            .STRATEGY_VARIABLES = StrategyVariables{
                .OHLC_RSI_VARIABLES = OHLCRSIVariables{.RSI_LONG = 60, .RSI_SHORT = 40},
            },
        },
    };

    const nlohmann::json j = config;
    return RedisLoader::load(j.dump(), env::getOr("REDIS_HOST", "127.0.0.1"));
}
