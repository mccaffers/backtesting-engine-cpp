#include "PriceRecord.h"
#include <chrono>
#include <iomanip>
#include <sstream>

// Constructor: Initializes a PriceRecord with UTC time string, ask price, and bid price
PriceRecord::PriceRecord(const std::string& utc, double askPrice, double bidPrice)
    : timestamp(parseUTC(utc)), askPrice(askPrice), bidPrice(bidPrice) {}

// Returns the timestamp as a chrono time_point
std::chrono::system_clock::time_point PriceRecord::getTimestamp() const {
    return timestamp;
}

// Converts the stored timestamp back to a UTC string
std::string PriceRecord::getUTC() const {
    // Convert time_point to time_t
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    std::stringstream ss;
    
    // Format the time as a string
    // std::gmtime is a function from the C standard library that converts a 
    //given time in seconds (represented by a time_t value) into a struct tm 
    // that represents the corresponding time in UTC (Coordinated Universal Time, 
    // formerly known as Greenwich Mean Time or GMT).
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

// Returns the ask price
double PriceRecord::getAskPrice() const {
    return askPrice;
}

// Returns the bid price
double PriceRecord::getBidPrice() const {
    return bidPrice;
}

// Parses a UTC string into a chrono time_point
std::chrono::system_clock::time_point PriceRecord::parseUTC(const std::string& utc) {
    std::tm tm = {};  // Initialize to all zeros
    std::istringstream ss(utc);
    // Parse the UTC string
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    // Convert tm to time_t, then to chrono time_point
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}