#ifndef STRATEGY_H
#define STRATEGY_H

#include "PriceRecord.h"
#include <vector>

class Strategy {
public:

    // Virtual destructor for the Strategy class
    // Destructors are special member functions that are called when an object is destroyed or goes out of scope.
    // They are responsible for cleaning up any resources that the object may have acquired during its lifetime
    // When you have a base class (like Strategy) and derived classes (like various strategies), and you're using polymorphism (i.e., handling derived class objects through base class pointers), you need a virtual destructor in the base class to clean up objects that may have been accquired during it's lifetime
    virtual ~Strategy() = default;


    // Trading decision
    virtual bool shouldTrade(const std::vector<PriceRecord>& priceHistory) = 0;
};

#endif // STRATEGY_H
