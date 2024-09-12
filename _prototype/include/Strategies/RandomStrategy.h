#pragma once

#include "IStrategy.h"
#include <random>

class RandomStrategy : public IStrategy {
private:
    std::mt19937 gen; // Mersenne Twister random number generator
    std::uniform_real_distribution<> dis; // Uniform distribution
    double tradeProbability; // Probability of making a trade

public:
    RandomStrategy(double tradeProbability = 0.5, unsigned seed = std::random_device{}());
    bool invoke(const std::vector<PriceRecord>& priceHistory) override;
};