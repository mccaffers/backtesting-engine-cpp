#include <gtest/gtest.h>
#include "PriceRecord.h"

TEST(PriceRecordTest, ConstructorAndGetters) {
    PriceRecord record("2023-09-02 10:00:00", 100.5, 100.0);
    
    EXPECT_EQ(record.getUTC(), "2023-09-02 10:00:00");
    EXPECT_DOUBLE_EQ(record.getAskPrice(), 100.5);
    EXPECT_DOUBLE_EQ(record.getBidPrice(), 100.0);
}
