#ifndef CSVPARSER_H
#define CSVPARSER_H

#include <string>               // For std::string
#include <vector>               // For std::vector
#include "PriceRecord.h"        // Custom class representing a price record

/**
 * @class CSVParser
 * @brief Provides functionality to parse CSV files containing price data.
 *
 * This class is responsible for reading a CSV file and converting its contents
 * into a collection of PriceRecord objects. It's designed to handle files
 * that contain price data in a specific format.
 */
class CSVParser {
public:
    /**
     * @brief Parses a CSV file and returns a vector of PriceRecord objects.
     * 
     * @param filePath The path to the CSV file to be parsed.
     * @return std::vector<PriceRecord> A vector containing PriceRecord objects,
     *         each representing a row from the CSV file.
     * 
     * This method reads the CSV file specified by filePath, parses its contents,
     * and creates a PriceRecord object for each row of data. The resulting
     * collection of PriceRecords is returned as a vector.
     * 
     * @note The exact format of the CSV file (column order, date format, etc.)
     *       should be documented elsewhere or in the implementation file.
     * 
     * @throws Potential exceptions (e.g., std::runtime_error) might be thrown
     *         if the file cannot be opened or if there's an error during parsing.
     */
    std::vector<PriceRecord> parseFile(const std::string& filePath);
};

#endif // CSVPARSER_H