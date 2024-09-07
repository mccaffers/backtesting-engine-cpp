#include "CSVParser.h"
#include <fstream>
#include <sstream>
#include <iostream>

std::vector<PriceRecord> CSVParser::parseFile(const std::string& filePath) {
    std::ifstream file(filePath);
    std::vector<PriceRecord> records;

    if (!file) {
        std::cerr << "Error: Could not open the file." << std::endl;
        return records;
    }

    std::string line;
    bool firstLine = true;
    
    while (std::getline(file, line)) {
        if (firstLine) {
            firstLine = false;  // Skip the header line
            continue;
        }

        std::stringstream ss(line);
        std::string utc, askPriceStr, bidPriceStr;

        std::getline(ss, utc, ',');
        std::getline(ss, askPriceStr, ',');
        std::getline(ss, bidPriceStr, ',');

        double askPrice = std::stod(askPriceStr);
        double bidPrice = std::stod(bidPriceStr);

        PriceRecord record(utc, askPrice, bidPrice);
        records.push_back(record);
    }

    file.close();
    return records;
}
