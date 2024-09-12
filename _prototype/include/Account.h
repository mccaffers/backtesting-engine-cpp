// Account.h
#ifndef ACCOUNT_H
#define ACCOUNT_H

#include <vector>
#include <string>
#include <optional>
#include <stdexcept>

struct Trade {
    std::string timestamp;
    double price;
    bool isBuy;
    std::optional<double> stopLoss;
    std::optional<double> limit;
    bool isOpen;
};

class Account {
private:
    double balance;
    std::vector<Trade> tradeHistory;
    std::optional<Trade> openTrade;

public:
    explicit Account(double initialBalance);
    void executeTrade(const std::string& timestamp, double price, bool isBuy, double stopLoss, double limit);
    void updateTrade(const std::string& timestamp, double currentPrice);
    double getBalance() const;
    const std::vector<Trade>& getTradeHistory() const;
    bool hasOpenTrade() const;
};

#endif // ACCOUNT_H