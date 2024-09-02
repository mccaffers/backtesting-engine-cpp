#include <iostream>
#include <vector>
#include <memory>
#include "CSVParser.h"
#include "PriceRecord.h"
#include "Strategy.h"
#include "Account.h"
#include "SimpleMovingAverageStrategy.h"

int main() {

    // Printing out the CPP version for debugging
    if (__cplusplus == 202101L) std::cout << "C++23";
    else if (__cplusplus == 202002L) std::cout << "C++20";
    else if (__cplusplus == 201703L) std::cout << "C++17";
    else if (__cplusplus == 201402L) std::cout << "C++14";
    else if (__cplusplus == 201103L) std::cout << "C++11";
    else if (__cplusplus == 199711L) std::cout << "C++98";
    else std::cout << "pre-standard C++." << __cplusplus;
    std::cout << "\n";

    // Get the file path from the environment variable
    const char* filePath = std::getenv("DATA_FILE_PATH");

    // Check if the environment variable is set
    if (filePath == nullptr) {
        std::cerr << "Error: DATA_FILE_PATH environment variable is not set." << std::endl;
        return 1;
    }

   CSVParser parser;
    std::vector<PriceRecord> records = parser.parseFile(filePath);

    // Reading the price line by line
    // for (const auto& record : records) {
    //     std::cout << "UTC: " << record.getUTC()
    //               << ", Ask Price: " << record.getAskPrice()
    //               << ", Bid Price: " << record.getBidPrice()
    //               << std::endl;
    // }

    // Create a strategy
    std::unique_ptr<Strategy> strategy = std::make_unique<SimpleMovingAverageStrategy>(10, 0.01);

    // Create an account with initial balance of 10000
    Account account(10000.0);

    // Simulate trading
    for (size_t i = 10; i < records.size(); ++i) {
        std::vector<PriceRecord> priceHistory(records.begin() + i - 10, records.begin() + i + 1);
        
        if (strategy->shouldTrade(priceHistory)) {
            // For simplicity, we're always buying in this example
            account.executeTrade(records[i].getUTC(), records[i].getAskPrice(), true);
            std::cout << "Executed trade at " << records[i].getUTC() << " for price " << records[i].getAskPrice() << std::endl;
        }
    }

    // Print final account balance and trade history
    std::cout << "Final account balance: " << account.getBalance() << std::endl;
    std::cout << "Trade history:" << std::endl;
    for (const auto& trade : account.getTradeHistory()) {
        std::cout << "Time: " << trade.timestamp 
                  << ", Price: " << trade.price 
                  << ", Action: " << (trade.isBuy ? "Buy" : "Sell") << std::endl;
    }

    return 0;
}

