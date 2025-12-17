/*
 * cache.hpp - Sector and Entry Caching for LevelFS
 * 
 * Compile: Include in mount.cpp with #include "cache.hpp"
 * 
 * Features:
 *   - Sector-level caching with LRU eviction
 *   - Write-through policy (writes go to disk immediately)
 *   - Dirty tracking for write-back optimization
 *   - Cache statistics tracking
 */

#ifndef CACHE_HPP
#define CACHE_HPP

#include <unordered_map>
#include <list>
#include <vector>
#include <cstring>
#include <cstdint>
#include <iostream>

using namespace std;

struct CacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
    
    CacheStats() : hits(0), misses(0), evictions(0), writebacks(0) {}
    
    double hitRate() const {
        uint64_t total = hits + misses;
        return total > 0 ? (double)hits / total * 100.0 : 0.0;
    }
    
    void display() const {
        cout << "\n=== Cache Statistics ===\n";
        cout << "  Hits:       " << hits << "\n";
        cout << "  Misses:     " << misses << "\n";
        cout << "  Hit Rate:   " << hitRate() << "%\n";
        cout << "  Evictions:  " << evictions << "\n";
        cout << "  Writebacks: " << writebacks << "\n";
    }
    
    void reset() {
        hits = misses = evictions = writebacks = 0;
    }
};

struct CacheEntry {
    vector<uint8_t> data;
    bool dirty;
    
    CacheEntry() : dirty(false) {}
    CacheEntry(size_t size) : data(size, 0), dirty(false) {}
};

template<size_t ENTRY_SIZE>
class LRUCache {
private:
    size_t maxEntries;
    
    list<uint64_t> lruList;
    unordered_map<uint64_t, typename list<uint64_t>::iterator> lruMap;
    unordered_map<uint64_t, CacheEntry> cache;
    
    CacheStats stats;
    
    void evictOldest() {
        if (lruList.empty()) return;
        
        uint64_t oldest = lruList.back();
        
        if (cache.count(oldest) && cache[oldest].dirty) {
            stats.writebacks++;
        }
        
        lruList.pop_back();
        lruMap.erase(oldest);
        cache.erase(oldest);
        stats.evictions++;
    }
    
    void touch(uint64_t key) {
        if (lruMap.count(key)) {
            lruList.erase(lruMap[key]);
        }
        lruList.push_front(key);
        lruMap[key] = lruList.begin();
    }

public:
    LRUCache(size_t maxSize = 1024) : maxEntries(maxSize) {}
    
    bool get(uint64_t key, void* buffer) {
        if (!cache.count(key)) {
            stats.misses++;
            return false;
        }
        
        stats.hits++;
        touch(key);
        memcpy(buffer, cache[key].data.data(), ENTRY_SIZE);
        return true;
    }
    
    void put(uint64_t key, const void* buffer, bool isDirty = false) {
        if (!cache.count(key) && cache.size() >= maxEntries) {
            evictOldest();
        }
        
        if (!cache.count(key)) {
            cache[key] = CacheEntry(ENTRY_SIZE);
        }
        
        memcpy(cache[key].data.data(), buffer, ENTRY_SIZE);
        cache[key].dirty = isDirty;
        touch(key);
    }
    
    void invalidate(uint64_t key) {
        if (cache.count(key)) {
            if (lruMap.count(key)) {
                lruList.erase(lruMap[key]);
                lruMap.erase(key);
            }
            cache.erase(key);
        }
    }
    
    void markDirty(uint64_t key) {
        if (cache.count(key)) {
            cache[key].dirty = true;
        }
    }
    
    bool isDirty(uint64_t key) const {
        auto it = cache.find(key);
        return it != cache.end() && it->second.dirty;
    }
    
    void markClean(uint64_t key) {
        if (cache.count(key)) {
            cache[key].dirty = false;
        }
    }
    
    void clear() {
        lruList.clear();
        lruMap.clear();
        cache.clear();
    }
    
    size_t size() const { return cache.size(); }
    size_t capacity() const { return maxEntries; }
    const CacheStats& getStats() const { return stats; }
    void resetStats() { stats.reset(); }
    
    void setMaxEntries(size_t max) {
        maxEntries = max;
        while (cache.size() > maxEntries) {
            evictOldest();
        }
    }
    
    vector<pair<uint64_t, bool>> getDirtyEntries() const {
        vector<pair<uint64_t, bool>> dirty;
        for (const auto& kv : cache) {
            if (kv.second.dirty) {
                dirty.push_back({kv.first, true});
            }
        }
        return dirty;
    }
};

class CachedDiskDevice {
private:
    void* realDisk;
    bool (*realReadSector)(void*, uint64_t, void*);
    bool (*realWriteSector)(void*, uint64_t, const void*);
    
    LRUCache<512> sectorCache;
    bool cacheEnabled;
    bool verbose;
    
public:
    CachedDiskDevice() : realDisk(nullptr), realReadSector(nullptr), 
                         realWriteSector(nullptr), sectorCache(2048),
                         cacheEnabled(true), verbose(false) {}
    
    void init(void* disk,
              bool (*readFn)(void*, uint64_t, void*),
              bool (*writeFn)(void*, uint64_t, const void*)) {
        realDisk = disk;
        realReadSector = readFn;
        realWriteSector = writeFn;
    }
    
    bool readSector(uint64_t sector, void* buffer) {
        if (!cacheEnabled || !realReadSector) {
            return realReadSector ? realReadSector(realDisk, sector, buffer) : false;
        }
        
        if (sectorCache.get(sector, buffer)) {
            if (verbose) {
                cout << "[Cache HIT] Sector " << sector << "\n";
            }
            return true;
        }
        
        if (!realReadSector(realDisk, sector, buffer)) {
            return false;
        }
        
        if (verbose) {
            cout << "[Cache MISS] Sector " << sector << " - loaded from disk\n";
        }
        
        sectorCache.put(sector, buffer, false);
        return true;
    }
    
    bool writeSector(uint64_t sector, const void* buffer) {
        if (!realWriteSector) return false;
        
        if (!realWriteSector(realDisk, sector, buffer)) {
            return false;
        }
        
        if (cacheEnabled) {
            sectorCache.put(sector, buffer, false);
        }
        
        return true;
    }
    
    void invalidate(uint64_t sector) {
        sectorCache.invalidate(sector);
    }
    
    void invalidateRange(uint64_t start, uint64_t count) {
        for (uint64_t i = 0; i < count; i++) {
            sectorCache.invalidate(start + i);
        }
    }
    
    void flush() {
        auto dirty = sectorCache.getDirtyEntries();
        for (const auto& entry : dirty) {
            uint8_t buffer[512];
            if (sectorCache.get(entry.first, buffer)) {
                realWriteSector(realDisk, entry.first, buffer);
                sectorCache.markClean(entry.first);
            }
        }
    }
    
    void clearCache() {
        flush();
        sectorCache.clear();
    }
    
    void enableCache(bool enable) { cacheEnabled = enable; }
    bool isCacheEnabled() const { return cacheEnabled; }
    
    void setVerbose(bool v) { verbose = v; }
    
    void setCacheSize(size_t entries) {
        sectorCache.setMaxEntries(entries);
    }
    
    const CacheStats& getStats() const { return sectorCache.getStats(); }
    void displayStats() const { sectorCache.getStats().display(); }
    void resetStats() { sectorCache.resetStats(); }
    
    size_t getCacheSize() const { return sectorCache.size(); }
    size_t getCacheCapacity() const { return sectorCache.capacity(); }
};

class SectorCache {
private:
    LRUCache<512> cache;
    bool enabled;
    
public:
    SectorCache(size_t maxEntries = 2048) : cache(maxEntries), enabled(true) {}
    
    bool read(uint64_t sector, void* buffer) {
        if (!enabled) return false;
        return cache.get(sector, buffer);
    }
    
    void write(uint64_t sector, const void* buffer) {
        if (enabled) {
            cache.put(sector, buffer, false);
        }
    }
    
    void invalidate(uint64_t sector) {
        cache.invalidate(sector);
    }
    
    void clear() { cache.clear(); }
    void enable(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }
    
    const CacheStats& stats() const { return cache.getStats(); }
    void displayStats() const { cache.getStats().display(); }
};

#endif
