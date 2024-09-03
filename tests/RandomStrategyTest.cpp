#include <gtest/gtest.h>
#include "RandomStrategy.h"
#include <vector>

class RandomStrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // This will be called before each test
    }

    void TearDown() override {
        // This will be called after each test
    }
};

TEST_F(RandomStrategyTest, ConstructorInitialization) {
    RandomStrategy strategy(0.5, 42);  // 50% trade probability, seed 42
    // We can't directly test private members, but we can test the behavior
}

TEST_F(RandomStrategyTest, ShouldTradeRespondsToTradeProbability) {
    const int numTrials = 10000;
    const double tradeProbability = 0.3;  // 30% trade probability
    RandomStrategy strategy(tradeProbability, 42);  // Fixed seed for reproducibility
    
    int tradeCount = 0;
    std::vector<PriceRecord> dummyHistory;  // Empty history, as it's not used by RandomStrategy
    
    for (int i = 0; i < numTrials; ++i) {
        if (strategy.shouldTrade(dummyHistory)) {
            ++tradeCount;
        }
    }
    
    double observedProbability = static_cast<double>(tradeCount) / numTrials;
    
    // Check if the observed probability is within 2% of the expected probability
    EXPECT_NEAR(observedProbability, tradeProbability, 0.02);
}

TEST_F(RandomStrategyTest, DifferentSeedsProduceDifferentResults) {
    RandomStrategy strategy1(0.5, 42);
    RandomStrategy strategy2(0.5, 43);
    
    std::vector<PriceRecord> dummyHistory;
    bool allSame = true;
    
    for (int i = 0; i < 100; ++i) {
        if (strategy1.shouldTrade(dummyHistory) != strategy2.shouldTrade(dummyHistory)) {
            allSame = false;
            break;
        }
    }
    
    EXPECT_FALSE(allSame);
}

TEST_F(RandomStrategyTest, ConsistentResultsWithSameSeed) {
    RandomStrategy strategy1(0.5, 42);
    RandomStrategy strategy2(0.5, 42);
    
    std::vector<PriceRecord> dummyHistory;
    bool allSame = true;
    
    for (int i = 0; i < 100; ++i) {
        if (strategy1.shouldTrade(dummyHistory) != strategy2.shouldTrade(dummyHistory)) {
            allSame = false;
            break;
        }
    }
    
    EXPECT_TRUE(allSame);
}