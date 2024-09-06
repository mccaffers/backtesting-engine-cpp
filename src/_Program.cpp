#include "Application.h"
#include <iostream>
#include <thread>
#include <chrono>

void printColorfulStartupMessage() {
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

int main() {
    Utils::printCppVersion();
    printColorfulStartupMessage();
    return Application().run();
}