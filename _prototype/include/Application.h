// Application.h
#pragma once

#include <iostream>
#include <vector>
#include <memory>
#include "CSVParser.h"
#include "PriceRecord.h"
#include "Account.h"
#include "CSVStream.h"
#include "Utils.h"
#include "IStrategy.h"
#include "IStrategyFactory.h"

class Application {
public:
    Application(std::unique_ptr<IStrategyFactory> factory);
    int run();

private:
    void processTrading();
    void printTradeHistory() const;

    std::unique_ptr<CSVStream> csvStream;
    std::unique_ptr<Account> account;
    std::vector<PriceRecord> priceHistory;
    const size_t historySize = 100;
    std::unique_ptr<IStrategy> strategy;
};
