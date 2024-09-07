// main.hpp
#ifndef MAIN_HPP
#define MAIN_HPP

#include <vector>

class Main {
public:
    static int addNumbers(const std::vector<int>& numbers) {
        int sum = 0;
        for (int num : numbers) {
            sum += num;
        }
        return sum;
    }
};

#endif // MAIN_HPP
