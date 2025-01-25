// Backtesting Engine in C++
//
// (c) 2025 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <iostream>
#include <memory>

class ConfigManager {
private:
    ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    
public:
    static std::shared_ptr<ConfigManager> getInstance() {
        static std::shared_ptr<ConfigManager> instance = std::shared_ptr<ConfigManager>(new ConfigManager());
        return instance;
    }
    
    std::string getConfig() const {
        return "config data"; // Replace with actual implementation
    }
};
