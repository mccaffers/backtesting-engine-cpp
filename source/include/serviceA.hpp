// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once
#include <iostream>
#include "configManager.hpp"

class ServiceA {
private:
    std::shared_ptr<ConfigManager> configManager;

public:
  explicit ServiceA(std::shared_ptr<ConfigManager> cm = ConfigManager::getInstance())
        : configManager(cm) {}

    void doSomething() const {
        std::cout << "ServiceA using config: " << configManager->getConfig() << std::endl;
    }
};

