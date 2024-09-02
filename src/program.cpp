#include <iostream>
#include <vector>
#include "CSVParser.h"
#include "PriceRecord.h"

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

    for (const auto& record : records) {
        std::cout << "UTC: " << record.getUTC()
                  << ", Ask Price: " << record.getAskPrice()
                  << ", Bid Price: " << record.getBidPrice()
                  << std::endl;
    }

    return 0;
}