#include "Account.h"

Account::Account(double initialBalance) : balance(initialBalance) {}

void Account::executeTrade(const std::string& timestamp, double price, bool isBuy) {
    Trade trade{timestamp, price, isBuy};
    tradeHistory.push_back(trade);

    if (isBuy) {
        balance -= price;
    } else {
        balance += price;
    }
}

double Account::getBalance() const {
    return balance;
}

const std::vector<Trade>& Account::getTradeHistory() const {
    return tradeHistory;
}