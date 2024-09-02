#ifndef STRATEGY_H
#define STRATEGY_H

#include "PriceRecord.h"
#include <vector>

class Strategy {
public:
    virtual bool shouldTrade(const std::vector<PriceRecord>& priceHistory) = 0;
    virtual ~Strategy() = default;
};

#endif // STRATEGY_H
