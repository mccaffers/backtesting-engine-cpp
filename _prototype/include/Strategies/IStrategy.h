#ifndef STRATEGY_H
#define STRATEGY_H

#include "PriceRecord.h"
#include <vector>

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual bool invoke(const std::vector<PriceRecord>& priceHistory) = 0;
};

#endif // STRATEGY_H