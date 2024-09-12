#include "Utils.h"
#include <iostream>

void Utils::printCppVersion() {

    std::cerr << "problematicMethod called from " << __FILE__ << ":" << __LINE__ << std::endl;

    const char* reset = "\033[0m";
    const char* bold = "\033[1m";
    const char* red = "\033[31m";
    const char* green = "\033[32m";
    const char* yellow = "\033[33m";
    const char* blue = "\033[34m";
    const char* magenta = "\033[35m";
    const char* cyan = "\033[36m";

    std::cout << std::endl;
    std::cout << bold << blue << "Systems Information" << reset << std::endl;
    std::cout << std::endl;

    std::cout << "C++ version: ";
    if (__cplusplus == 202101L) std::cout << "C++23";
    else if (__cplusplus == 202002L) std::cout << "C++20";
    else if (__cplusplus == 201703L) std::cout << "C++17";
    else if (__cplusplus == 201402L) std::cout << "C++14";
    else if (__cplusplus == 201103L) std::cout << "C++11";
    else if (__cplusplus == 199711L) std::cout << "C++98";
    else std::cout << "pre-standard C++." << __cplusplus;
    std::cout << "\n";

    const char* filePath = std::getenv("DATA_FILE_PATH");
    if (filePath == nullptr) {
        throw std::runtime_error("DATA_FILE_PATH environment variable is not set.1");
    }
    
    std::cout << "File " << filePath << std::endl;
}

void Utils::printColorfulStartupMessage() {
    // ANSI color codes
    const char* reset = "\033[0m";
    const char* bold = "\033[1m";
    const char* red = "\033[31m";
    const char* green = "\033[32m";
    const char* yellow = "\033[33m";
    const char* blue = "\033[34m";
    const char* magenta = "\033[35m";
    const char* cyan = "\033[36m";

    std::cout << std::endl;
    std::cout << bold << blue << "Starting Backtesting Engine" << reset << std::endl;
    std::cout << std::endl;

}

