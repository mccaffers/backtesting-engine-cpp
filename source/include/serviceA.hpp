// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <iostream>
#include "ConfigManager.hpp"

class ServiceA {
private:
    std::shared_ptr<ConfigManager> configManager;

public:
    ServiceA(std::shared_ptr<ConfigManager> cm) : configManager(cm) {}

    void doSomething() {
        std::cout << "ServiceA using config: " << configManager->getConfig() << std::endl;
    }
};
