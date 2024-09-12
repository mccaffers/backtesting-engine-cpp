#include "Account.h"

Account::Account(double initialBalance) : balance(initialBalance), openTrade(std::nullopt) {}

void Account::executeTrade(const std::string& timestamp, double price, bool isBuy, double stopLoss, double limit) {
    if (hasOpenTrade()) {
        throw std::runtime_error("Cannot execute new trade while another trade is open");
    }

    Trade trade{timestamp, price, isBuy, stopLoss, limit, true};
    openTrade = trade;
    tradeHistory.push_back(trade);

    if (isBuy) {
        balance -= price;
    } else {
        balance += price;
    }
}

void Account::updateTrade(const std::string& timestamp, double currentPrice) {
    if (!hasOpenTrade()) {
        return;
    }

    Trade& trade = openTrade.value();
    bool shouldClose = false;

    if (trade.isBuy) {
        if (currentPrice <= trade.stopLoss.value() || currentPrice >= trade.limit.value()) {
            shouldClose = true;
        }
    } else {
        if (currentPrice >= trade.stopLoss.value() || currentPrice <= trade.limit.value()) {
            shouldClose = true;
        }
    }

    if (shouldClose) {
        double profit;
        if (trade.isBuy) {
            profit = currentPrice - trade.price;
        } else {
            profit = trade.price - currentPrice;
        }
        balance += profit; // Only add the profit/loss to the balance
        trade.isOpen = false;
        trade.price = currentPrice;  // Update the closing price
        trade.timestamp = timestamp; // Update the closing timestamp
        tradeHistory.back() = trade; // Update the trade in history
        openTrade = std::nullopt;    // Clear the open trade
    }
}

double Account::getBalance() const {
    return balance;
}

const std::vector<Trade>& Account::getTradeHistory() const {
    return tradeHistory;
}

bool Account::hasOpenTrade() const {
    return openTrade.has_value();
}