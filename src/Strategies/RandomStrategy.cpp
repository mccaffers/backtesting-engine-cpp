#include "RandomStrategy.h"

RandomStrategy::RandomStrategy(double tradeProbability, unsigned seed)
    : gen(seed), dis(0.0, 1.0), tradeProbability(tradeProbability) {}

// vector: This is a standard C++ container that represents a dynamic array. It can grow or shrink in size as needed.
// <PriceRecord>: This syntax specifies that the vector contains elements of type PriceRecord.
// &: This ampersand at the end indicates that this is a reference to a vector, not a vector itself.
bool RandomStrategy::shouldTrade(const std::vector<PriceRecord>& priceHistory) {
    
    // Generate a random number between 0 and 1
    double randomValue = dis(gen);
    
    // Decide to trade if the random value is less than the trade probability
    return randomValue < tradeProbability;
}
