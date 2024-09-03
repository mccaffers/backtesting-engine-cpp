#ifndef CSV_STREAM_H
#define CSV_STREAM_H

#include <fstream>              // For file input operations
#include <queue>                // For storing PriceRecords in a buffer
#include <mutex>                // For thread-safe operations on shared data
#include <condition_variable>   // For synchronizing threads
#include "PriceRecord.h"        // Custom class representing a price record

/**
 * @class CSVStream
 * @brief Provides a thread-safe stream interface for reading CSV data.
 *
 * This class allows for asynchronous reading of CSV data, buffering PriceRecords,
 * and providing a thread-safe interface for consuming those records.
 */
class CSVStream {
private:
    std::ifstream file;              ///< Input file stream for reading the CSV file
    std::queue<PriceRecord> buffer;  ///< Buffer to store parsed PriceRecords
    std::mutex mtx;                  ///< Mutex for thread-safe access to the buffer
    std::condition_variable cv;      ///< Condition variable for synchronizing producer and consumer
    bool done = false;               ///< Flag indicating whether all data has been read

public:
    /**
     * @brief Constructor that takes a filename.
     * @param filename The path to the CSV file to be read.
     */
    CSVStream(const std::string& filename);

    /**
     * @brief Starts the asynchronous reading of the CSV file.
     * 
     * This method likely spawns a separate thread to read and parse the CSV data.
     */
    void start();

    /**
     * @brief Retrieves the next PriceRecord from the buffer.
     * @return The next available PriceRecord.
     * 
     * This method is likely blocking if no data is available and not finished.
     */
    PriceRecord getNext();

    /**
     * @brief Checks if the CSV file has been fully read and processed.
     * @return true if all data has been read, false otherwise.
     */
    bool isFinished();
};

#endif // CSV_STREAM_H