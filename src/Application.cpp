#include "Application.h"

Application::Application() 
    : strategy(std::make_unique<RandomStrategy>(0.3)),
      account(std::make_unique<Account>(10000.0)) {
    
    const char* filePath = std::getenv("DATA_FILE_PATH");
    if (filePath == nullptr) {
        throw std::runtime_error("DATA_FILE_PATH environment variable is not set.");
    }
    
    csvStream = std::make_unique<CSVStream>(filePath);
    csvStream->start();
}

int Application::run() {
    // try {
    //     processTrading();
    //     std::cout << "Final account balance: " << account->getBalance() << std::endl;
    //     printTradeHistory();
    // } catch (const std::exception& e) {
    //     std::cerr << "Error: " << e.what() << std::endl;
    //     return 1;
    // }
    return 0;
}

void Application::processTrading() {
    while (!csvStream->isFinished()) {
        PriceRecord record = csvStream->getNext();
        priceHistory.push_back(record);
        if (priceHistory.size() > historySize) {
            priceHistory.erase(priceHistory.begin());
        }

        account->updateTrade(record.getUTC(), record.getAskPrice());

        if (!account->hasOpenTrade() && strategy->shouldTrade(priceHistory)) {
            double currentPrice = record.getAskPrice();
            double stopLoss = currentPrice * 0.99; // 1% stop loss
            double limit = currentPrice * 1.02; // 2% take profit
            account->executeTrade(record.getUTC(), currentPrice, true, stopLoss, limit);
            std::cout << "Executed trade at " << record.getUTC()
                      << " for price " << currentPrice
                      << " Stop Loss: " << stopLoss
                      << " Limit: " << limit
                      << " Balance: " << account->getBalance() << std::endl;
        }
    }
}

void Application::printTradeHistory() const {
    std::cout << "Trade history:" << std::endl;
    for (const auto& trade : account->getTradeHistory()) {
        std::cout << "Time: " << trade.timestamp
                  << ", Price: " << trade.price
                  << ", Action: " << (trade.isBuy ? "Buy" : "Sell")
                  << ", Stop Loss: " << trade.stopLoss.value_or(-1)
                  << ", Limit: " << trade.limit.value_or(-1)
                  << ", Status: " << (trade.isOpen ? "Open" : "Closed")
                  << std::endl;
    }
}