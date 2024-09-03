#include "PriceRecord.h"

PriceRecord::PriceRecord(const std::string& utc, double askPrice, double bidPrice)
    : utc(utc), askPrice(askPrice), bidPrice(bidPrice) {}

std::string PriceRecord::getUTC() const {
    return utc;
}

double PriceRecord::getAskPrice() const {
    return askPrice;
}

double PriceRecord::getBidPrice() const {
    return bidPrice;
}
