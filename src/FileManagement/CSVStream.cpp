#include "CSVStream.h"
#include <sstream>
#include <thread>
#include <iostream>

CSVStream::CSVStream(const std::string& filename) : file(filename) {
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        fileOpenSuccessful = false;
    } else {
        fileOpenSuccessful = true;
    }
}

void CSVStream::start() {

    if (!fileOpenSuccessful) {
        std::cerr << "Cannot start processing. File was not opened successfully." << std::endl;
        return;
    }

    std::thread([this]() {
        std::string line;
        std::getline(file, line); // Skip header
        std::cout << "Header line: " << line << std::endl;

        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string utc, askPriceStr, bidPriceStr;

            std::getline(ss, utc, ',');
            std::getline(ss, askPriceStr, ',');
            std::getline(ss, bidPriceStr, ',');

            double askPrice = std::stod(askPriceStr);
            double bidPrice = std::stod(bidPriceStr);

            PriceRecord record(utc, askPrice, bidPrice);

            {
                std::lock_guard<std::mutex> lock(mtx);
                buffer.push(record);
            }
            cv.notify_one();
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            done = true;
        }
        cv.notify_all();
    }).detach();
}

PriceRecord CSVStream::getNext() {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [this] { return !buffer.empty() || done; });

    if (!buffer.empty()) {
        PriceRecord record = buffer.front();
        buffer.pop();
        return record;
    }

    throw std::runtime_error("No more records");
}

bool CSVStream::isFinished() {
    std::lock_guard<std::mutex> lock(mtx);
    return done && buffer.empty();
}