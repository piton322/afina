#pragma once

#include <map>
#include <mutex>
#include <string>

#include "SimpleLRU.h"

namespace Afina {
namespace Backend {

/**
 * # SimpleLRU thread safe version
 *
 *
 */
class ThreadSafeSimplLRU : public SimpleLRU {
public:
    ThreadSafeSimplLRU(size_t max_size = 1024) : SimpleLRU(max_size) {}
    ~ThreadSafeSimplLRU() {}

    // see SimpleLRU.h
    bool Put(const std::string & key, const std::string & value) override {
        // sinchronization

        // lock_guard vs unique_lock: 
        // The difference is that you can lock and unlock 
        // a std::unique_lock. std::lock_guard will be 
        // locked only once on construction and unlocked on destruction.
        std::unique_lock<std::mutex> lock(mutex_lru);
        return SimpleLRU::Put(key, value);
    }

    // see SimpleLRU.h
    bool PutIfAbsent(const std::string & key, const std::string & value) override {
        // sinchronization
        std::unique_lock<std::mutex> lock(mutex_lru);
        return SimpleLRU::PutIfAbsent(key, value);
    }

    // see SimpleLRU.h
    bool Set(const std::string & key, const std::string & value) override {
        // sinchronization
        std::unique_lock<std::mutex> lock(mutex_lru);
        return SimpleLRU::Set(key, value);
    }

    // see SimpleLRU.h
    bool Delete(const std::string & key) override {
        // sinchronization
        std::unique_lock<std::mutex> lock(mutex_lru);
        return SimpleLRU::Delete(key);
    }

    // see SimpleLRU.h
    bool Get(const std::string & key, std::string & value) override {
        // sinchronization
        std::unique_lock<std::mutex> lock(mutex_lru);
        return SimpleLRU::Get(key, value);
    }

private:
    // sinchronization primitives
    std::mutex mutex_lru;
};

} // namespace Backend
} // namespace Afina
