#include <gtest/gtest.h>
#include "Account.h"

class AccountTest : public ::testing::Test {
protected:
    void SetUp() override {
        // This will be called before each test
        std::cout << "SetUp called" << std::endl;
    }

    void TearDown() override {
        // This will be called after each test
        std::cout << "Tear Down called" << std::endl;
    }
};

TEST_F(AccountTest, ConstructorInitialization) {
    Account account(1000.0);
    EXPECT_DOUBLE_EQ(account.getBalance(), 1000.0);
    EXPECT_TRUE(account.getTradeHistory().empty());
}

TEST_F(AccountTest, ExecuteBuyTrade) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true);
    
    EXPECT_DOUBLE_EQ(account.getBalance(), 900.0);
    ASSERT_EQ(account.getTradeHistory().size(), 1);
    EXPECT_EQ(account.getTradeHistory()[0].timestamp, "2023-09-04 10:00:00");
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].price, 100.0);
    EXPECT_TRUE(account.getTradeHistory()[0].isBuy);
}

TEST_F(AccountTest, ExecuteSellTrade) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 11:00:00", 150.0, false);
    
    EXPECT_DOUBLE_EQ(account.getBalance(), 1150.0);
    ASSERT_EQ(account.getTradeHistory().size(), 1);
    EXPECT_EQ(account.getTradeHistory()[0].timestamp, "2023-09-04 11:00:00");
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].price, 150.0);
    EXPECT_FALSE(account.getTradeHistory()[0].isBuy);
}

TEST_F(AccountTest, MultipleTrades) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true);
    account.executeTrade("2023-09-04 11:00:00", 150.0, false);
    account.executeTrade("2023-09-04 12:00:00", 75.0, true);
    
    EXPECT_DOUBLE_EQ(account.getBalance(), 975.0);
    ASSERT_EQ(account.getTradeHistory().size(), 3);
}

TEST_F(AccountTest, TradeHistoryOrder) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true);
    account.executeTrade("2023-09-04 11:00:00", 150.0, false);
    
    const auto& history = account.getTradeHistory();
    ASSERT_EQ(history.size(), 2);
    EXPECT_EQ(history[0].timestamp, "2023-09-04 10:00:00");
    EXPECT_EQ(history[1].timestamp, "2023-09-04 11:00:00");
}