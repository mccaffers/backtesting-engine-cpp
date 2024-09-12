#include "EnvironmentStrategyFactory.h"
#include "Strategies.h"
#include <cstdlib>
#include <stdexcept>
#include <iostream>

std::unique_ptr<IStrategy> EnvironmentStrategyFactory::createStrategy() {
    std::cout << "Create Strategy called" << std::endl;

    const char* env = std::getenv("STRATEGY_TYPE");
    std::string strategyType = env ? env : "A";

    if (strategyType == "A") {
        return std::make_unique<StrategyA>();
    } else if (strategyType == "B") {
        return std::make_unique<StrategyB>();
    } else {
        throw std::runtime_error("Unknown strategy type");
    }
}
