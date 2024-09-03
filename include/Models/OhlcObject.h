#include <chrono>   // For std::chrono::system_clock::time_point
#include <limits>   // For std::numeric_limits

/**
 * @class OhlcObject
 * @brief Represents an OHLC (Open, High, Low, Close) data point in financial markets.
 *
 * This class stores information about a single candlestick in a financial chart,
 * including the opening price, closing price, highest price, lowest price,
 * the date/time of the candlestick, and whether the candlestick is complete.
 */
class OhlcObject {
public:
    /**
     * @brief Default constructor for OhlcObject.
     *
     * Initializes all members with default values:
     * - date: Set to the minimum possible time point
     * - open and close: Set to 0.0
     * - high: Set to the lowest possible double value
     * - low: Set to the highest possible double value
     * - complete: Set to false
     */
    OhlcObject() :
        date(std::chrono::system_clock::time_point::min()),
        open(0.0),
        close(0.0),
        high(std::numeric_limits<double>::lowest()),
        low(std::numeric_limits<double>::max()),
        complete(false) {}

    std::chrono::system_clock::time_point date;  ///< The date and time of this OHLC data point
    double open;    ///< The opening price for this time period
    double close;   ///< The closing price for this time period
    double high;    ///< The highest price reached during this time period
    double low;     ///< The lowest price reached during this time period
    bool complete;  ///< Indicates whether this candlestick is complete (true) or still in progress (false)
};

#endif // OHLC_OBJECT_H