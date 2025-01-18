#include "strategy_variables.hpp"

namespace trading_definitions {

void to_json(nlohmann::json& j, const StrategyVariables& s) {
    if (s.OHLC_RSI_VARIABLES) {
        j["OHLC_RSI_VARIABLES"] = *s.OHLC_RSI_VARIABLES;
    } else {
        j["OHLC_RSI_VARIABLES"] = nullptr;
    }
}

void from_json(const nlohmann::json& j, StrategyVariables& s) {
    if (j.contains("OHLC_RSI_VARIABLES") && !j.at("OHLC_RSI_VARIABLES").is_null()) {
        s.OHLC_RSI_VARIABLES = j.at("OHLC_RSI_VARIABLES").get<OHLCRSIVariables>();
    } else {
        s.OHLC_RSI_VARIABLES = std::nullopt;
    }
}

} // namespace strategy
