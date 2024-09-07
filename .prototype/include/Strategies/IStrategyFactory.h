#pragma once
#include "IStrategy.h"
#include <memory>

class IStrategyFactory {
public:
    virtual std::unique_ptr<IStrategy> createStrategy() = 0;
    virtual ~IStrategyFactory() = default;
};