#ifndef PRICERECORD_H
#define PRICERECORD_H

#include <string>

class PriceRecord {
private:
    std::string utc;
    double askPrice;
    double bidPrice;

public:
    // Constructor
    PriceRecord(const std::string& utc, double askPrice, double bidPrice);

    // Getters
    std::string getUTC() const;
    double getAskPrice() const;
    double getBidPrice() const;
};

#endif
