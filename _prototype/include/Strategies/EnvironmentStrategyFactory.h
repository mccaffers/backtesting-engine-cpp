#pragma once
#include "IStrategyFactory.h"

class EnvironmentStrategyFactory : public IStrategyFactory {
public:
    std::unique_ptr<IStrategy> createStrategy() override;
};