#include "SimpleMovingAverageStrategy.h"
#include <numeric>

SimpleMovingAverageStrategy::SimpleMovingAverageStrategy(int period, double threshold)
    : period(period), threshold(threshold) {}

bool SimpleMovingAverageStrategy::shouldTrade(const std::vector<PriceRecord>& priceHistory) {
    if (priceHistory.size() < period) return false;

    auto start = priceHistory.end() - period;
    auto end = priceHistory.end();

    double sum = std::accumulate(start, end, 0.0, 
        [](double acc, const PriceRecord& record) { return acc + record.getAskPrice(); });
    
    double sma = sum / period;
    double currentPrice = priceHistory.back().getAskPrice();

    return (currentPrice - sma) / sma > threshold;
}
