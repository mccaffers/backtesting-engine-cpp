#ifndef CSV_STREAM_H
#define CSV_STREAM_H

#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "PriceRecord.h"

class CSVStream {
private:
    std::ifstream file;
    std::queue<PriceRecord> buffer;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

public:
    CSVStream(const std::string& filename);
    void start();
    PriceRecord getNext();
    bool isFinished();
};

#endif // CSV_STREAM_H