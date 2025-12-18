/*
 * cache.hpp - Advanced Caching Layer for LevelFS
 * 
 * Compile: Included automatically via fs_common.hpp
 * 
 * Features:
 *   - Sector-level LRU caching with configurable size
 *   - Cluster-level caching for directory entries
 *   - Write-through policy (immediate persistence)
 *   - Cache statistics tracking
 *   - Automatic eviction management
 */

#ifndef CACHE_HPP
#define CACHE_HPP

#include <unordered_map>
#include <list>
#include <vector>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <mutex>

using namespace std;

struct LfsCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writes;
    uint64_t writebacks;
    
    LfsCacheStats() : hits(0), misses(0), evictions(0), writes(0), writebacks(0) {}
    
    double hitRate() const {
        uint64_t total = hits + misses;
        return total > 0 ? (double)hits / total * 100.0 : 0.0;
    }
    
    void reset() { hits = misses = evictions = writes = writebacks = 0; }
};

template<size_t ENTRY_SIZE>
class LfsLRUCache {
public:
    using WriteCallback = bool(*)(uint64_t key, const void* data, size_t size, void* context);

private:
    size_t maxEntries;
    list<uint64_t> lruList;
    unordered_map<uint64_t, typename list<uint64_t>::iterator> lruMap;
    unordered_map<uint64_t, vector<uint8_t>> cacheData;
    unordered_map<uint64_t, bool> dirtyBits;
    LfsCacheStats stats;
    mutex mtx;
    bool enabled;
    
    WriteCallback writeCallback;
    void* callbackContext;
    
    void evictOldestUnsafe() {
        if (lruList.empty()) return;
        uint64_t oldest = lruList.back();
        
        if (dirtyBits.count(oldest) && dirtyBits[oldest]) {
            if (writeCallback && cacheData.count(oldest)) {
                writeCallback(oldest, cacheData[oldest].data(), ENTRY_SIZE, callbackContext);
                stats.writebacks++;
            }
            dirtyBits.erase(oldest);
        }
        
        lruList.pop_back();
        lruMap.erase(oldest);
        cacheData.erase(oldest);
        stats.evictions++;
    }
    
    void touchUnsafe(uint64_t key) {
        if (lruMap.count(key)) {
            lruList.erase(lruMap[key]);
        }
        lruList.push_front(key);
        lruMap[key] = lruList.begin();
    }

public:
    LfsLRUCache(size_t maxSize = 2048) : maxEntries(maxSize), enabled(true),
                                         writeCallback(nullptr), callbackContext(nullptr) {}
    
    bool get(uint64_t key, void* buffer) {
        lock_guard<mutex> guard(mtx);
        
        if (!enabled) return false;
        
        auto it = cacheData.find(key);
        if (it == cacheData.end()) {
            stats.misses++;
            return false;
        }
        
        stats.hits++;
        touchUnsafe(key);
        memcpy(buffer, it->second.data(), ENTRY_SIZE);
        return true;
    }
    
    void put(uint64_t key, const void* buffer, bool isDirty = false) {
        lock_guard<mutex> guard(mtx);
        
        if (!enabled) return;
        
        if (!cacheData.count(key) && cacheData.size() >= maxEntries) {
            evictOldestUnsafe();
        }
        
        if (!cacheData.count(key)) {
            cacheData[key] = vector<uint8_t>(ENTRY_SIZE);
        }
        
        memcpy(cacheData[key].data(), buffer, ENTRY_SIZE);
        if (isDirty) dirtyBits[key] = true;
        touchUnsafe(key);
        stats.writes++;
    }
    
    void invalidate(uint64_t key) {
        lock_guard<mutex> guard(mtx);
        
        auto it = cacheData.find(key);
        if (it != cacheData.end()) {
            if (lruMap.count(key)) {
                lruList.erase(lruMap[key]);
                lruMap.erase(key);
            }
            cacheData.erase(it);
        }
    }
    
    void invalidateRange(uint64_t start, uint64_t count) {
        lock_guard<mutex> guard(mtx);
        
        for (uint64_t i = 0; i < count; i++) {
            uint64_t key = start + i;
            auto it = cacheData.find(key);
            if (it != cacheData.end()) {
                if (lruMap.count(key)) {
                    lruList.erase(lruMap[key]);
                    lruMap.erase(key);
                }
                cacheData.erase(it);
            }
        }
    }
    
    void clear() {
        lock_guard<mutex> guard(mtx);
        lruList.clear();
        lruMap.clear();
        cacheData.clear();
    }
    
    void enable(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }
    
    size_t size() const { return cacheData.size(); }
    size_t capacity() const { return maxEntries; }
    
    void setCapacity(size_t cap) {
        lock_guard<mutex> guard(mtx);
        maxEntries = cap;
        while (cacheData.size() > maxEntries) {
            evictOldestUnsafe();
        }
    }
    
    const LfsCacheStats& getStats() const { return stats; }
    void resetStats() { stats.reset(); }
    
    void setWriteCallback(WriteCallback cb, void* ctx) {
        writeCallback = cb;
        callbackContext = ctx;
    }
    
    void markDirty(uint64_t key) {
        lock_guard<mutex> guard(mtx);
        if (cacheData.count(key)) {
            dirtyBits[key] = true;
        }
    }
    
    void markClean(uint64_t key) {
        lock_guard<mutex> guard(mtx);
        dirtyBits.erase(key);
    }
    
    size_t getDirtyCount() const {
        size_t count = 0;
        for (const auto& kv : dirtyBits) {
            if (kv.second) count++;
        }
        return count;
    }
    
    size_t flush() {
        lock_guard<mutex> guard(mtx);
        size_t flushed = 0;
        
        if (!writeCallback) return 0;
        
        for (auto& kv : dirtyBits) {
            if (kv.second && cacheData.count(kv.first)) {
                if (writeCallback(kv.first, cacheData[kv.first].data(), ENTRY_SIZE, callbackContext)) {
                    kv.second = false;
                    flushed++;
                    stats.writebacks++;
                }
            }
        }
        
        dirtyBits.clear();
        return flushed;
    }
    
    void displayStats() const {
        cout << "\n=== Cache Statistics ===\n";
        cout << "  Status:     " << (enabled ? "ENABLED" : "DISABLED") << "\n";
        cout << "  Entries:    " << cacheData.size() << " / " << maxEntries << "\n";
        cout << "  Dirty:      " << getDirtyCount() << "\n";
        cout << "  Hits:       " << stats.hits << "\n";
        cout << "  Misses:     " << stats.misses << "\n";
        cout << "  Hit Rate:   " << fixed << setprecision(1) << stats.hitRate() << "%\n";
        cout << "  Evictions:  " << stats.evictions << "\n";
        cout << "  Writebacks: " << stats.writebacks << "\n";
    }
};

class LfsSectorCache {
public:
    using WriteCallback = LfsLRUCache<512>::WriteCallback;

private:
    static const size_t SECTOR_SIZE = 512;
    LfsLRUCache<SECTOR_SIZE> cache;
    
public:
    LfsSectorCache(size_t maxSectors = 4096) : cache(maxSectors) {}
    
    bool read(uint64_t sector, void* buffer) {
        return cache.get(sector, buffer);
    }
    
    void write(uint64_t sector, const void* buffer, bool isDirty = false) {
        cache.put(sector, buffer, isDirty);
    }
    
    void invalidate(uint64_t sector) {
        cache.invalidate(sector);
    }
    
    void invalidateCluster(uint64_t cluster, size_t sectorsPerCluster = 8) {
        cache.invalidateRange(cluster * sectorsPerCluster, sectorsPerCluster);
    }
    
    void clear() { cache.clear(); }
    void enable(bool e) { cache.enable(e); }
    bool isEnabled() const { return cache.isEnabled(); }
    
    size_t size() const { return cache.size(); }
    size_t capacity() const { return cache.capacity(); }
    void setCapacity(size_t cap) { cache.setCapacity(cap); }
    
    void setWriteCallback(WriteCallback cb, void* ctx) { cache.setWriteCallback(cb, ctx); }
    void markDirty(uint64_t sector) { cache.markDirty(sector); }
    void markClean(uint64_t sector) { cache.markClean(sector); }
    size_t getDirtyCount() const { return cache.getDirtyCount(); }
    size_t flush() { return cache.flush(); }
    
    const LfsCacheStats& stats() const { return cache.getStats(); }
    void resetStats() { cache.resetStats(); }
    void displayStats() const { cache.displayStats(); }
};

class LfsClusterCache {
private:
    static const size_t CLUSTER_SIZE = 4096;
    LfsLRUCache<CLUSTER_SIZE> cache;
    
public:
    LfsClusterCache(size_t maxClusters = 512) : cache(maxClusters) {}
    
    bool read(uint64_t cluster, void* buffer) {
        return cache.get(cluster, buffer);
    }
    
    void write(uint64_t cluster, const void* buffer) {
        cache.put(cluster, buffer);
    }
    
    void invalidate(uint64_t cluster) {
        cache.invalidate(cluster);
    }
    
    void clear() { cache.clear(); }
    void enable(bool e) { cache.enable(e); }
    bool isEnabled() const { return cache.isEnabled(); }
    
    size_t size() const { return cache.size(); }
    size_t capacity() const { return cache.capacity(); }
    
    const LfsCacheStats& stats() const { return cache.getStats(); }
    void resetStats() { cache.resetStats(); }
    void displayStats() const { cache.displayStats(); }
};

#endif
