#ifndef RANDOM_STRATEGY_H
#define RANDOM_STRATEGY_H

#include "Strategy.h"
#include <random>

class RandomStrategy : public Strategy {
private:
    std::mt19937 gen; // Mersenne Twister random number generator
    std::uniform_real_distribution<> dis; // Uniform distribution
    double tradeProbability; // Probability of making a trade

public:
    RandomStrategy(double tradeProbability = 0.5, unsigned seed = std::random_device{}());
    bool shouldTrade(const std::vector<PriceRecord>& priceHistory) override;
};

#endif // RANDOM_STRATEGY_H
