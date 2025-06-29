#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <mutex>
#include <memory>

#include "Types.h"

#include <spdlog/spdlog.h>

namespace motioncam {

class LRUCache {
public:
    explicit LRUCache(size_t maxSize) : mMaxSize(maxSize), mCurrentSize(0) {}

    // Get value from cache, returns nullptr if not found
    // If another thread is already processing the same key, this thread will wait
    std::shared_ptr<std::vector<char>> get(const Entry& key, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock<std::mutex> lock(mMutex);

        // Wait if another thread is currently processing this key, with timeout
        bool success = mCondition.wait_for(lock, timeout, [this, &key] {
            return mInProgress.find(key) == mInProgress.end();
        });

        if (!success) {
            // Timeout occurred - another thread is taking too long
            spdlog::warn("Timeout waiting for key to be processed by another thread");
            return nullptr;
        }

        auto it = mCacheMap.find(key);
        if (it == mCacheMap.end()) {
            // Cache miss - mark as in progress so other threads wait
            mInProgress.insert(key);
            lock.unlock();

            // Notify that this key is now being processed
            // (Other threads will wait in the condition above)

            // Return nullptr to indicate cache miss
            // The caller should handle loading the data and calling put()
            return nullptr;
        }

        // Cache hit, move to front of list (most recently used)
        mCacheList.splice(mCacheList.begin(), mCacheList, it->second);

        return it->second->second;
    }

    // Add or update value in cache
    void put(const Entry& key, std::shared_ptr<std::vector<char>> value) {
        std::lock_guard<std::mutex> lock(mMutex);

        size_t valueSize = value->size();

        // Check if key already exists in cache
        auto it = mCacheMap.find(key);

        if (it != mCacheMap.end()) {
            // Update value
            size_t oldSize = it->second->second->size();
            mCurrentSize -= oldSize;
            mCurrentSize += valueSize;

            // Move to front and update
            mCacheList.splice(mCacheList.begin(), mCacheList, it->second);
            it->second->second = value;
        }
        else {
            // New entry

            // If adding this would exceed max size, remove older entries
            while (!mCacheList.empty() && (mCurrentSize + valueSize > mMaxSize)) {
                auto last = mCacheList.back();
                mCurrentSize -= last.second->size();
                mCacheMap.erase(last.first);
                mCacheList.pop_back();
            }

            // If the single item is too large for the cache, don't add it
            if (valueSize > mMaxSize) {
                // Remove from in-progress set and notify waiting threads
                mInProgress.erase(key);
                mCondition.notify_all();
                return;
            }

            // Add new entry
            mCacheList.emplace_front(key, value);
            mCacheMap[key] = mCacheList.begin();
            mCurrentSize += valueSize;
        }

        // Remove from in-progress set and notify waiting threads
        mInProgress.erase(key);
        mCondition.notify_all();

        spdlog::debug("Cache size is {} bytes", mCurrentSize);
    }

    // Remove an entry from the cache
    void remove(const Entry& key) {
        std::lock_guard<std::mutex> lock(mMutex);

        auto it = mCacheMap.find(key);

        if (it != mCacheMap.end()) {
            mCurrentSize -= it->second->second->size();
            mCacheList.erase(it->second);
            mCacheMap.erase(it);
        }

        // Also remove from in-progress set if present and notify
        if (mInProgress.erase(key) > 0) {
            mCondition.notify_all();
        }
    }

    // Clear the cache
    void clear() {
        std::lock_guard<std::mutex> lock(mMutex);

        mCacheMap.clear();
        mCacheList.clear();
        mInProgress.clear();
        mCurrentSize = 0;
        mCondition.notify_all();
    }

    // Get current size
    size_t size() const {
        std::lock_guard<std::mutex> lock(mMutex);

        return mCurrentSize;
    }

    // Get maximum size
    size_t capacity() const {
        return mMaxSize;
    }

    // Method to mark that processing for a key has failed
    // This should be called if the caller gets nullptr from get() but fails to load the data
    void markLoadFailed(const Entry& key) {
        std::lock_guard<std::mutex> lock(mMutex);
        mInProgress.erase(key);
        mCondition.notify_all();
    }

private:
    using CacheItem = std::pair<Entry, std::shared_ptr<std::vector<char>>>;
    using CacheList = std::list<CacheItem>;
    using CacheMap = std::unordered_map<Entry, typename CacheList::iterator, Entry::Hash>;

    CacheList mCacheList; // List of cache entries, most recently used at the front
    CacheMap mCacheMap;   // Map from key to list iterator
    std::unordered_set<Entry, Entry::Hash> mInProgress; // Set of keys currently being processed
    size_t mMaxSize;      // Maximum cache size in bytes
    size_t mCurrentSize;  // Current cache size in bytes
    mutable std::mutex mMutex; // Mutex for thread safety
    mutable std::condition_variable mCondition; // Condition variable for waiting
};

}
