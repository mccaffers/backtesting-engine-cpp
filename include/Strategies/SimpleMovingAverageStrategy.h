#ifndef SIMPLE_MOVING_AVERAGE_STRATEGY_H
#define SIMPLE_MOVING_AVERAGE_STRATEGY_H

#include "Strategy.h"
#include <vector>

class SimpleMovingAverageStrategy : public Strategy {
private:
    int period;
    double threshold;

public:
    SimpleMovingAverageStrategy(int period, double threshold);
    bool shouldTrade(const std::vector<PriceRecord>& priceHistory) override;
};

#endif // SIMPLE_MOVING_AVERAGE_STRATEGY_H
