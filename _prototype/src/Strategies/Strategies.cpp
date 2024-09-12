// Strategies.cpp
#include "Strategies.h"
#include <iostream>
#include "PriceRecord.h"

bool StrategyA::invoke(const std::vector<PriceRecord>& priceHistory)  {
    std::cout << "Executing Strategy A" << std::endl;
    return 0;
}

bool StrategyB::invoke(const std::vector<PriceRecord>& priceHistory)  {
    std::cout << "Executing Strategy B" << std::endl;
    return 0;
}