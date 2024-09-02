#include "RandomStrategy.h"

RandomStrategy::RandomStrategy(double tradeProbability, unsigned seed)
    : gen(seed), dis(0.0, 1.0), tradeProbability(tradeProbability) {}

bool RandomStrategy::shouldTrade(const std::vector<PriceRecord>& priceHistory) {
    // Generate a random number between 0 and 1
    double randomValue = dis(gen);
    
    // Decide to trade if the random value is less than the trade probability
    return randomValue < tradeProbability;
}
