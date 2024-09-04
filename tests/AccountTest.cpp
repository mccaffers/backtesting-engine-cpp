#include <gtest/gtest.h>
#include "Account.h"

class AccountTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::cout << "SetUp called" << std::endl;
    }
    void TearDown() override {
        std::cout << "Tear Down called" << std::endl;
    }
};

TEST_F(AccountTest, ConstructorInitialization) {
    Account account(1000.0);
    EXPECT_DOUBLE_EQ(account.getBalance(), 1000.0);
    EXPECT_TRUE(account.getTradeHistory().empty());
    EXPECT_FALSE(account.hasOpenTrade());
}

TEST_F(AccountTest, ExecuteBuyTrade) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true, 99.0, 101.0);
    EXPECT_DOUBLE_EQ(account.getBalance(), 900.0);
    ASSERT_EQ(account.getTradeHistory().size(), 1);
    EXPECT_EQ(account.getTradeHistory()[0].timestamp, "2023-09-04 10:00:00");
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].price, 100.0);
    EXPECT_TRUE(account.getTradeHistory()[0].isBuy);
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].stopLoss.value(), 99.0);
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].limit.value(), 101.0);
    EXPECT_TRUE(account.getTradeHistory()[0].isOpen);
    EXPECT_TRUE(account.hasOpenTrade());
}

TEST_F(AccountTest, ExecuteSellTrade) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 11:00:00", 150.0, false, 151.0, 149.0);
    EXPECT_DOUBLE_EQ(account.getBalance(), 1150.0);
    ASSERT_EQ(account.getTradeHistory().size(), 1);
    EXPECT_EQ(account.getTradeHistory()[0].timestamp, "2023-09-04 11:00:00");
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].price, 150.0);
    EXPECT_FALSE(account.getTradeHistory()[0].isBuy);
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].stopLoss.value(), 151.0);
    EXPECT_DOUBLE_EQ(account.getTradeHistory()[0].limit.value(), 149.0);
    EXPECT_TRUE(account.getTradeHistory()[0].isOpen);
    EXPECT_TRUE(account.hasOpenTrade());
}

TEST_F(AccountTest, UpdateTradeStopLoss) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true, 99.0, 101.0);
    account.updateTrade("2023-09-04 10:01:00", 98.5);
    EXPECT_DOUBLE_EQ(account.getBalance(), 898.5);
    ASSERT_EQ(account.getTradeHistory().size(), 1);
    EXPECT_FALSE(account.getTradeHistory()[0].isOpen);
    EXPECT_FALSE(account.hasOpenTrade());
}

TEST_F(AccountTest, UpdateTradeLimit) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true, 99.0, 101.0);
    account.updateTrade("2023-09-04 10:01:00", 101.5);
    EXPECT_DOUBLE_EQ(account.getBalance(), 901.5);
    ASSERT_EQ(account.getTradeHistory().size(), 1);
    EXPECT_FALSE(account.getTradeHistory()[0].isOpen);
    EXPECT_FALSE(account.hasOpenTrade());
}

TEST_F(AccountTest, CannotExecuteMultipleOpenTrades) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true, 99.0, 101.0);
    EXPECT_THROW(account.executeTrade("2023-09-04 10:01:00", 102.0, true, 101.0, 103.0), std::runtime_error);
}

TEST_F(AccountTest, TradeHistoryOrder) {
    Account account(1000.0);
    account.executeTrade("2023-09-04 10:00:00", 100.0, true, 99.0, 101.0);
    account.updateTrade("2023-09-04 10:01:00", 101.5);
    account.executeTrade("2023-09-04 11:00:00", 150.0, false, 151.0, 149.0);
    const auto& history = account.getTradeHistory();
    ASSERT_EQ(history.size(), 2);
    EXPECT_EQ(history[0].timestamp, "2023-09-04 10:01:00");  // Updated timestamp
    EXPECT_EQ(history[1].timestamp, "2023-09-04 11:00:00");
}