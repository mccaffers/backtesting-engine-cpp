////#include <gtest/gtest.h>
////#include <gtest.h>
//#include <gtest/gtest.h>
//#include "main.hpp"
//
//TEST(AddNumbersTest, EmptyVector) {
//    std::vector<int> numbers;
//    EXPECT_EQ(Main::addNumbers(numbers), 0);
//}
//
//TEST(AddNumbersTest, SingleNumber) {
//    std::vector<int> numbers = {5};
//    EXPECT_EQ(Main::addNumbers(numbers), 5);
//}
//
//TEST(AddNumbersTest, MultipleNumbers) {
//    std::vector<int> numbers = {1, 2, 3, 4, 5};
//    EXPECT_EQ(Main::addNumbers(numbers), 15);
//}
//
//TEST(AddNumbersTest, NegativeNumbers) {
//    std::vector<int> numbers = {-1, -2, 3, 4, -5};
//    EXPECT_EQ(Main::addNumbers(numbers), -1);
//}
//
//TEST(AddNumbersTest, LargeNumbers) {
//    std::vector<int> numbers = {1000000, 2000000, 3000000};
//    EXPECT_EQ(Main::addNumbers(numbers), 6000000);
//}
//
//int main(int argc, char **argv) {
//    testing::InitGoogleTest(&argc, argv);
//    return RUN_ALL_TESTS();
//}
