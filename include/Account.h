#ifndef ACCOUNT_H
#define ACCOUNT_H

#include <vector>
#include <string>

struct Trade {
    std::string timestamp;
    double price;
    bool isBuy;
};

class Account {
private:
    double balance;
    std::vector<Trade> tradeHistory;

public:
    Account(double initialBalance);
    void executeTrade(const std::string& timestamp, double price, bool isBuy);
    double getBalance() const;
    const std::vector<Trade>& getTradeHistory() const;
};

#endif // ACCOUNT_H
