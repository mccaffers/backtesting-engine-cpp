#ifndef CSVPARSER_H
#define CSVPARSER_H

#include <string>
#include <vector>
#include "PriceRecord.h"

class CSVParser {
public:
    std::vector<PriceRecord> parseFile(const std::string& filePath);
};

#endif
