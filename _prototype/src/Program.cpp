#include <iostream>
#include <thread>
#include <chrono>
#include "Application.h"
#include "EnvironmentStrategyFactory.h"

int main() {

    const char* consoleMessages = std::getenv("CONSOLE_MESSAGES");
    if (consoleMessages != nullptr) {
        Utils::printCppVersion();
        Utils::printColorfulStartupMessage();
    }

    auto factory = std::make_unique<EnvironmentStrategyFactory>();
    Application app(std::move(factory));
    app.run();

    return 0;
}
