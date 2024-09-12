// Backtesting Engine in C++
//
// (c) 2024 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
#pragma once
#include <iostream>

class ConfigManager {
private:
    static std::shared_ptr<ConfigManager> instance;
    std::string config;

    ConfigManager() : config("Default Configuration") {}

public:
    static std::shared_ptr<ConfigManager> getInstance() {
        if (!instance) {
            instance = std::shared_ptr<ConfigManager>(new ConfigManager());
        }
        return instance;
    }

    void setConfig(const std::string& newConfig) {
        config = newConfig;
    }

    std::string getConfig() const {
        return config;
    }
};
