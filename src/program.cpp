#include <iostream>
#include <vector>
#include <memory>
#include "CSVParser.h"
#include "PriceRecord.h"
#include "Strategy.h"
#include "Account.h"
#include "CSVStream.h"
#include "RandomStrategy.h"


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

    CSVStream csvStream(filePath);
    csvStream.start();

    std::unique_ptr<Strategy> strategy = std::make_unique<RandomStrategy>(0.3);
    Account account(10000.0);

    std::vector<PriceRecord> priceHistory;
    const size_t historySize = 100; // Adjust based on strategy needs

    while (!csvStream.isFinished()) {
        try {
            PriceRecord record = csvStream.getNext();
            priceHistory.push_back(record);
            if (priceHistory.size() > historySize) {
                priceHistory.erase(priceHistory.begin());
            }

            if (strategy->shouldTrade(priceHistory)) {
                account.executeTrade(record.getUTC(), record.getAskPrice(), true);
                std::cout << "Executed trade at " << record.getUTC() 
                          << " for price " << record.getAskPrice() << std::endl;
            }
        } catch (const std::runtime_error& e) {
            break;
        }
    }

    std::cout << "Final account balance: " << account.getBalance() << std::endl;
    
    std::cout << "Trade history:" << std::endl;
    for (const auto& trade : account.getTradeHistory()) {
        std::cout << "Time: " << trade.timestamp 
                  << ", Price: " << trade.price 
                  << ", Action: " << (trade.isBuy ? "Buy" : "Sell") << std::endl;
    }

    return 0;
}

