// Application.h
#pragma once

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

class Application {
public:
    Application();
    int run();

private:
    void processTrading();
    void printTradeHistory() const;

    std::unique_ptr<CSVStream> csvStream;
    std::unique_ptr<Strategy> strategy;
    std::unique_ptr<Account> account;
    std::vector<PriceRecord> priceHistory;
    const size_t historySize = 100;
};
