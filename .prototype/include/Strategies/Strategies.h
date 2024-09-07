#pragma once
#include "IStrategy.h"

class StrategyA : public IStrategy {
public:
    bool invoke(const std::vector<PriceRecord>& priceHistory) override;
};

class StrategyB : public IStrategy {
public:
    bool invoke(const std::vector<PriceRecord>& priceHistory) override;
};