#include <gtest/gtest.h>
#include "PriceRecord.h"
#include <chrono>

TEST(PriceRecordTest, ConstructorAndGetters) {
    PriceRecord record("2023-09-02T10:00:00", 100.5, 100.0);

    // Test getUTC()
    EXPECT_EQ(record.getUTC(), "2023-09-02T10:00:00");

    // Test getAskPrice() and getBidPrice()
    EXPECT_DOUBLE_EQ(record.getAskPrice(), 100.5);
    EXPECT_DOUBLE_EQ(record.getBidPrice(), 100.0);

    // Test getTimestamp()
    auto timestamp = record.getTimestamp();
    auto timeT = std::chrono::system_clock::to_time_t(timestamp);
    struct tm* timeinfo = gmtime(&timeT);
    
    EXPECT_EQ(timeinfo->tm_year + 1900, 2023);
    EXPECT_EQ(timeinfo->tm_mon + 1, 9);     // tm_mon is 0-indexed
    EXPECT_EQ(timeinfo->tm_mday, 2);
    EXPECT_EQ(timeinfo->tm_hour, 10);
    EXPECT_EQ(timeinfo->tm_min, 0);
    EXPECT_EQ(timeinfo->tm_sec, 0);
}

// Additional test to ensure correct parsing of different UTC strings
TEST(PriceRecordTest, UTCParsing) {
    PriceRecord record1("2023-12-31T23:59:59", 200.0, 199.5);
    EXPECT_EQ(record1.getUTC(), "2023-12-31T23:59:59");

    PriceRecord record2("2024-01-01T00:00:00", 201.0, 200.5);
    EXPECT_EQ(record2.getUTC(), "2024-01-01T00:00:00");

    // Test with the format from your original file
    PriceRecord record3("2020-01-01T22:01:12.821+00:00", 1.1216, 1.12106);
    EXPECT_EQ(record3.getUTC(), "2020-01-01T22:01:12");
}