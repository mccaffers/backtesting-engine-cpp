#include <iostream>
#include <vector>
#include <memory>
#include "CSVParser.h"
#include "PriceRecord.h"
#include "Strategy.h"
#include "Account.h"
#include "CSVStream.h"
#include "RandomStrategy.h"
#include "Utils.h"

int main() {
    Utils::printCppVersion();

    const char* filePath = std::getenv("DATA_FILE_PATH");
    if (filePath == nullptr) {
        std::cerr << "Error: DATA_FILE_PATH environment variable is not set." << std::endl;
        return 1;
    }

    CSVStream csvStream(filePath);
    csvStream.start();

    std::unique_ptr<Strategy> strategy = std::make_unique<RandomStrategy>(0.3);
    Account account(10000.0);

    std::vector<PriceRecord> priceHistory;
    const size_t historySize = 100;

    while (!csvStream.isFinished()) {
        try {
            PriceRecord record = csvStream.getNext();
            priceHistory.push_back(record);
            if (priceHistory.size() > historySize) {
                priceHistory.erase(priceHistory.begin());
            }

            // Update any open trade
            account.updateTrade(record.getUTC(), record.getAskPrice());

            // If there's no open trade, consider opening a new one
            if (!account.hasOpenTrade() && strategy->shouldTrade(priceHistory)) {
                double currentPrice = record.getAskPrice();
                double stopLoss = currentPrice * 0.99;  // 1% stop loss
                double limit = currentPrice * 1.02;     // 2% take profit
                account.executeTrade(record.getUTC(), currentPrice, true, stopLoss, limit);
                std::cout << "Executed trade at " << record.getUTC() 
                          << " for price " << currentPrice 
                          << " Stop Loss: " << stopLoss
                          << " Limit: " << limit
                          << " Balance: " << account.getBalance() << std::endl;
            }
        } catch (const std::runtime_error& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            break;
        }
    }

    std::cout << "Final account balance: " << account.getBalance() << std::endl;
    
    std::cout << "Trade history:" << std::endl;
    for (const auto& trade : account.getTradeHistory()) {
        std::cout << "Time: " << trade.timestamp 
                  << ", Price: " << trade.price 
                  << ", Action: " << (trade.isBuy ? "Buy" : "Sell")
                  << ", Stop Loss: " << trade.stopLoss.value_or(-1)
                  << ", Limit: " << trade.limit.value_or(-1)
                  << ", Status: " << (trade.isOpen ? "Open" : "Closed")
                  << std::endl;
    }

    return 0;
}