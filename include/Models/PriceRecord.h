#ifndef PRICERECORD_H
#define PRICERECORD_H

#include <string>
#include <ctime>
#include <sstream>
#include <iomanip>

class PriceRecord {
private:
    std::chrono::system_clock::time_point timestamp;
    double askPrice;
    double bidPrice;

public:
    // Constructor
    PriceRecord(const std::string& utc, double askPrice, double bidPrice);

    // Getters
    std::chrono::system_clock::time_point getTimestamp() const;
    std::string getUTC() const;
    double getAskPrice() const;
    double getBidPrice() const;

private:
    // Helper function to parse UTC string
    static std::chrono::system_clock::time_point parseUTC(const std::string& utc);
};

#endif