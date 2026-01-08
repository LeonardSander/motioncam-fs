#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <mutex>
#include <memory>
#include <string>
#include <cmath>

#include <chrono>

#include "Types.h"

#include <spdlog/spdlog.h>

namespace motioncam {

class LRUCache {
public:
    explicit LRUCache(size_t maxSize) : mMaxSize(maxSize), mCurrentSize(0) {}
    
    void setStreamingBypass(bool enabled) {
        std::lock_guard<std::mutex> lock(mMutex);
        mStreamingBypass = enabled;
    }

    bool isStreamingBypassEnabled() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mStreamingBypass;
    }

    bool isStreamingBypassActive() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mStreamingBypass && mCurrentPlaybackFrame >= 0;
    }

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

        // Track playback position for smart caching (BEFORE checking cache)
        // This allows us to detect sequential playback even on cache misses
        int frameNum = extractFrameNumber(key);
        if (frameNum >= 0) {
            // Detect sequential access pattern (indicates playback)
            if (mLastAccessedFrame >= 0 && frameNum == mLastAccessedFrame + 1) {
                mSequentialCount++;
                if (mSequentialCount >= 3) {  // 3 sequential frames = playback detected
                    mCurrentPlaybackFrame = frameNum;
                    spdlog::info("[SMART CACHE] Playback detected at frame {} (sequential count: {})",
                                 frameNum, mSequentialCount);
                }
            } else if (frameNum != mLastAccessedFrame) {
                mSequentialCount = 0;  // Reset if not sequential
            }
            mLastAccessedFrame = frameNum;
        }

        const bool streamingActive = mStreamingBypass && mCurrentPlaybackFrame >= 0;
        if (streamingActive) {
            spdlog::info("[CACHE] BYPASS (streaming): {}", key.name);
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

        auto now = std::chrono::steady_clock::now();
        if (mTTL.count() > 0) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->lastAccessTime);
            if (age > mTTL) {
                mCurrentSize -= it->second->data->size();
                mCacheList.erase(it->second);
                mCacheMap.erase(it);

                // Mark as in progress for regeneration
                mInProgress.insert(key);
                lock.unlock();
                return nullptr;
            }
        }

        // Cache hit, move to front of list (most recently used)
        mCacheList.splice(mCacheList.begin(), mCacheList, it->second);
        it->second->lastAccessTime = now;

        return it->second->data;
    }

    // Add or update value in cache
    void put(const Entry& key, std::shared_ptr<std::vector<char>> value) {
        std::lock_guard<std::mutex> lock(mMutex);

        size_t valueSize = value->size();

        const bool streamingActive = mStreamingBypass && mCurrentPlaybackFrame >= 0;
        if (streamingActive) {
            mInProgress.erase(key);
            mCondition.notify_all();
            spdlog::info("[CACHE] BYPASS STORE (streaming): {}", key.name);
            return;
        }

        // Check if key already exists in cache
        auto it = mCacheMap.find(key);

        if (it != mCacheMap.end()) {
            // Update value
            size_t oldSize = it->second->data->size();
            mCurrentSize -= oldSize;
            mCurrentSize += valueSize;

            // Move to front and update
            mCacheList.splice(mCacheList.begin(), mCacheList, it->second);
            it->second->data = value;
            it->second->lastAccessTime = std::chrono::steady_clock::now();
        }
        else {
            // New entry
            size_t evictedCount = 0;
            size_t evictedBytes = 0;

            // If adding this would exceed max size, remove entries
            while (!mCacheList.empty() && (mCurrentSize + valueSize > mMaxSize)) {
                // Use playback-aware eviction if we have detected playback position
                if (mCurrentPlaybackFrame >= 0) {
                    // Find the frame furthest from current playback position
                    auto furthestIt = mCacheList.end();
                    int maxDistance = -1;

                    for (auto it = mCacheList.begin(); it != mCacheList.end(); ++it) {
                        int frameNum = extractFrameNumber(it->key);
                        if (frameNum >= 0) {
                            int distance = std::abs(frameNum - mCurrentPlaybackFrame);
                            if (distance > maxDistance) {
                                maxDistance = distance;
                                furthestIt = it;
                            }
                        }
                    }

                    if (furthestIt != mCacheList.end()) {
                        // Evict the furthest frame
                        size_t removedSize = furthestIt->data->size();
                        mCurrentSize -= removedSize;
                        evictedBytes += removedSize;
                        evictedCount++;
                        mCacheMap.erase(furthestIt->key);
                        mCacheList.erase(furthestIt);

                        if (evictedCount == 1) {
                            spdlog::warn("[SMART CACHE] Evicting frame furthest from playback position {} (distance: {})",
                                         mCurrentPlaybackFrame, maxDistance);
                        }
                    } else {
                        // Fall back to LRU if no frame numbers found
                        auto last = mCacheList.back();
                        size_t removedSize = last.data->size();
                        mCurrentSize -= removedSize;
                        evictedBytes += removedSize;
                        evictedCount++;
                        mCacheMap.erase(last.key);
                        mCacheList.pop_back();
                    }
                } else {
                    // Standard LRU eviction (oldest first)
                    auto last = mCacheList.back();
                    size_t removedSize = last.data->size();
                    mCurrentSize -= removedSize;
                    evictedBytes += removedSize;
                    evictedCount++;
                    mCacheMap.erase(last.key);
                    mCacheList.pop_back();
                }
            }

            if (evictedCount > 0 && mCurrentPlaybackFrame >= 0) {
                spdlog::warn("[SMART CACHE] Evicted {} entries ({} MB) around playback frame {}",
                            evictedCount, evictedBytes / (1024*1024), mCurrentPlaybackFrame);
            }

            // If the single item is too large for the cache, don't add it
            if (valueSize > mMaxSize) {
                // Remove from in-progress set and notify waiting threads
                mInProgress.erase(key);
                mCondition.notify_all();
                return;
            }

            // Add new entry
            CacheEntry newEntry{key, value, std::chrono::steady_clock::now()};
            mCacheList.emplace_front(newEntry);
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
            mCurrentSize -= it->second->data->size();
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

    void setTTL(std::chrono::seconds ttl) {
        std::lock_guard<std::mutex> lock(mMutex);
        mTTL = ttl;
    }

    std::chrono::seconds getTTL() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mTTL;
    }

    size_t cleanupExpired() {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mTTL.count() == 0) {
            return 0;
        }

        auto now = std::chrono::steady_clock::now();
        size_t removed = 0;

        auto it = mCacheList.begin();
        while (it != mCacheList.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->lastAccessTime);
            if (age > mTTL) {
                mCurrentSize -= it->data->size();
                mCacheMap.erase(it->key);
                it = mCacheList.erase(it);
                removed++;
            } else {
                ++it;
            }
        }

        if (removed > 0) {
            spdlog::debug("Cache cleanup: removed {} expired entries, size now {} bytes", removed, mCurrentSize);
        }
        return removed;
    }

    // Method to mark that processing for a key has failed
    // This should be called if the caller gets nullptr from get() but fails to load the data
    void markLoadFailed(const Entry& key) {
        std::lock_guard<std::mutex> lock(mMutex);
        mInProgress.erase(key);
        mCondition.notify_all();
    }

private:
    struct CacheEntry {
        Entry key;
        std::shared_ptr<std::vector<char>> data;
        std::chrono::steady_clock::time_point lastAccessTime;
    };
    using CacheItem = CacheEntry;
    using CacheList = std::list<CacheItem>;
    using CacheMap = std::unordered_map<Entry, typename CacheList::iterator, Entry::Hash>;

    // Extract frame number from entry name (e.g., "video-000123.dng" -> 123)
    int extractFrameNumber(const Entry& entry) const {
        const std::string& name = entry.name;
        size_t dashPos = name.rfind('-');
        size_t dotPos = name.rfind('.');

        if (dashPos != std::string::npos && dotPos != std::string::npos && dashPos < dotPos) {
            try {
                return std::stoi(name.substr(dashPos + 1, dotPos - dashPos - 1));
            } catch (...) {
                return -1;
            }
        }
        return -1;
    }

    CacheList mCacheList; // List of cache entries, most recently used at the front
    CacheMap mCacheMap;   // Map from key to list iterator
    std::unordered_set<Entry, Entry::Hash> mInProgress; // Set of keys currently being processed
    size_t mMaxSize;      // Maximum cache size in bytes
    size_t mCurrentSize;  // Current cache size in bytes
    std::chrono::seconds mTTL{0};  // 0 = disabled
    mutable std::mutex mMutex; // Mutex for thread safety
    mutable std::condition_variable mCondition; // Condition variable for waiting

    // Playback-aware caching
    int mCurrentPlaybackFrame = -1;  // Detected current playback position
    int mLastAccessedFrame = -1;     // Last frame that was accessed
    int mSequentialCount = 0;        // Counter for detecting sequential access
    bool mStreamingBypass = false;   // Bypass cache entirely during streaming playback
};

}
