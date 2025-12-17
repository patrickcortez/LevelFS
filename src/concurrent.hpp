/*
 * concurrent.hpp - Concurrent Access Handling for LevelFS
 * 
 * Compile: Include in mount.cpp with #include "concurrent.hpp"
 * 
 * Features:
 *   - File-level locks (exclusive write, shared read)
 *   - Record-level byte range locks
 *   - Lock timeout support
 *   - Named mutex for cross-process synchronization
 */

#ifndef CONCURRENT_HPP
#define CONCURRENT_HPP

#include <windows.h>
#include <unordered_map>
#include <map>
#include <set>
#include <string>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <mutex>

using namespace std;

enum LfsLockType {
    LFS_LOCK_NONE = 0,
    LFS_LOCK_SHARED = 1,
    LFS_LOCK_EXCLUSIVE = 2
};

enum LfsLockStatus {
    LFS_LOCK_ACQUIRED = 0,
    LFS_LOCK_DENIED = 1,
    LFS_LOCK_TIMEOUT = 2,
    LFS_LOCK_DEADLOCK = 3,
    LFS_LOCK_INVALID = 4
};

struct ByteRange {
    uint64_t start;
    uint64_t length;
    
    ByteRange() : start(0), length(0) {}
    ByteRange(uint64_t s, uint64_t l) : start(s), length(l) {}
    
    uint64_t end() const { return start + length; }
    
    bool overlaps(const ByteRange& other) const {
        return start < other.end() && other.start < end();
    }
};

struct FileLock {
    uint64_t fileCluster;
    string fileName;
    LfsLockType type;
    uint32_t ownerId;
    uint64_t acquireTime;
    
    FileLock() : fileCluster(0), type(LFS_LOCK_NONE), ownerId(0), acquireTime(0) {}
};

struct RangeLock {
    uint64_t fileCluster;
    ByteRange range;
    LfsLockType type;
    uint32_t ownerId;
    uint64_t acquireTime;
    
    RangeLock() : fileCluster(0), type(LFS_LOCK_NONE), ownerId(0), acquireTime(0) {}
};

class FileLockManager {
private:
    mutex mtx;
    unordered_map<uint64_t, vector<FileLock>> fileLocks;
    unordered_map<uint64_t, vector<RangeLock>> rangeLocks;
    uint32_t currentOwnerId;
    bool verbose;
    
    HANDLE systemMutex;
    string mutexName;
    
    bool hasConflictingFileLock(uint64_t cluster, LfsLockType requestedType, uint32_t requesterId) {
        if (!fileLocks.count(cluster)) return false;
        
        for (const auto& lock : fileLocks[cluster]) {
            if (lock.ownerId == requesterId) continue;
            
            if (lock.type == LFS_LOCK_EXCLUSIVE) return true;
            if (requestedType == LFS_LOCK_EXCLUSIVE && lock.type == LFS_LOCK_SHARED) return true;
        }
        return false;
    }
    
    bool hasConflictingRangeLock(uint64_t cluster, const ByteRange& range, 
                                  LfsLockType requestedType, uint32_t requesterId) {
        if (!rangeLocks.count(cluster)) return false;
        
        for (const auto& lock : rangeLocks[cluster]) {
            if (lock.ownerId == requesterId) continue;
            if (!lock.range.overlaps(range)) continue;
            
            if (lock.type == LFS_LOCK_EXCLUSIVE) return true;
            if (requestedType == LFS_LOCK_EXCLUSIVE && lock.type == LFS_LOCK_SHARED) return true;
        }
        return false;
    }

public:
    FileLockManager() : currentOwnerId(GetCurrentProcessId()), verbose(false), 
                        systemMutex(NULL) {
        mutexName = "Global\\LevelFS_Lock_";
    }
    
    ~FileLockManager() {
        releaseAllLocks(currentOwnerId);
        if (systemMutex) {
            ReleaseMutex(systemMutex);
            CloseHandle(systemMutex);
        }
    }
    
    void setVerbose(bool v) { verbose = v; }
    uint32_t getOwnerId() const { return currentOwnerId; }
    
    bool initSystemLock(const string& volumeId) {
        string fullName = mutexName + volumeId;
        systemMutex = CreateMutexA(NULL, FALSE, fullName.c_str());
        if (systemMutex == NULL) {
            if (verbose) cout << "[Lock] Failed to create system mutex\n";
            return false;
        }
        if (verbose) cout << "[Lock] System mutex created: " << fullName << "\n";
        return true;
    }
    
    LfsLockStatus acquireFileLock(uint64_t cluster, const string& name, 
                                LfsLockType type, uint32_t timeoutMs = 5000) {
        lock_guard<mutex> guard(mtx);
        
        if (type == LFS_LOCK_NONE) return LFS_LOCK_INVALID;
        
        auto startTime = chrono::steady_clock::now();
        
        while (hasConflictingFileLock(cluster, type, currentOwnerId)) {
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                chrono::steady_clock::now() - startTime).count();
            
            if (elapsed >= timeoutMs) {
                if (verbose) {
                    cout << "[Lock] TIMEOUT acquiring " << (type == LFS_LOCK_EXCLUSIVE ? "EXCLUSIVE" : "SHARED")
                         << " lock on '" << name << "'\n";
                }
                return LFS_LOCK_TIMEOUT;
            }
            
            Sleep(10);
        }
        
        FileLock newLock;
        newLock.fileCluster = cluster;
        newLock.fileName = name;
        newLock.type = type;
        newLock.ownerId = currentOwnerId;
        newLock.acquireTime = time(0);
        
        fileLocks[cluster].push_back(newLock);
        
        if (verbose) {
            cout << "[Lock] ACQUIRED " << (type == LFS_LOCK_EXCLUSIVE ? "EXCLUSIVE" : "SHARED")
                 << " lock on '" << name << "' (cluster " << cluster << ")\n";
        }
        
        return LFS_LOCK_ACQUIRED;
    }
    
    LfsLockStatus acquireRangeLock(uint64_t cluster, const ByteRange& range,
                                 LfsLockType type, uint32_t timeoutMs = 5000) {
        lock_guard<mutex> guard(mtx);
        
        if (type == LFS_LOCK_NONE) return LFS_LOCK_INVALID;
        
        auto startTime = chrono::steady_clock::now();
        
        while (hasConflictingRangeLock(cluster, range, type, currentOwnerId)) {
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                chrono::steady_clock::now() - startTime).count();
            
            if (elapsed >= timeoutMs) {
                if (verbose) {
                    cout << "[Lock] TIMEOUT acquiring range lock [" << range.start 
                         << "-" << range.end() << "]\n";
                }
                return LFS_LOCK_TIMEOUT;
            }
            
            Sleep(10);
        }
        
        RangeLock newLock;
        newLock.fileCluster = cluster;
        newLock.range = range;
        newLock.type = type;
        newLock.ownerId = currentOwnerId;
        newLock.acquireTime = time(0);
        
        rangeLocks[cluster].push_back(newLock);
        
        if (verbose) {
            cout << "[Lock] ACQUIRED " << (type == LFS_LOCK_EXCLUSIVE ? "EXCLUSIVE" : "SHARED")
                 << " range lock [" << range.start << "-" << range.end() << "]\n";
        }
        
        return LFS_LOCK_ACQUIRED;
    }
    
    bool releaseFileLock(uint64_t cluster, uint32_t ownerId = 0) {
        lock_guard<mutex> guard(mtx);
        
        if (ownerId == 0) ownerId = currentOwnerId;
        if (!fileLocks.count(cluster)) return false;
        
        auto& locks = fileLocks[cluster];
        for (auto it = locks.begin(); it != locks.end(); ++it) {
            if (it->ownerId == ownerId) {
                if (verbose) {
                    cout << "[Lock] RELEASED lock on '" << it->fileName << "'\n";
                }
                locks.erase(it);
                if (locks.empty()) fileLocks.erase(cluster);
                return true;
            }
        }
        return false;
    }
    
    bool releaseRangeLock(uint64_t cluster, const ByteRange& range, uint32_t ownerId = 0) {
        lock_guard<mutex> guard(mtx);
        
        if (ownerId == 0) ownerId = currentOwnerId;
        if (!rangeLocks.count(cluster)) return false;
        
        auto& locks = rangeLocks[cluster];
        for (auto it = locks.begin(); it != locks.end(); ++it) {
            if (it->ownerId == ownerId && it->range.start == range.start && 
                it->range.length == range.length) {
                if (verbose) {
                    cout << "[Lock] RELEASED range lock [" << range.start 
                         << "-" << range.end() << "]\n";
                }
                locks.erase(it);
                if (locks.empty()) rangeLocks.erase(cluster);
                return true;
            }
        }
        return false;
    }
    
    void releaseAllLocks(uint32_t ownerId = 0) {
        lock_guard<mutex> guard(mtx);
        
        if (ownerId == 0) ownerId = currentOwnerId;
        
        for (auto& kv : fileLocks) {
            auto& locks = kv.second;
            locks.erase(remove_if(locks.begin(), locks.end(),
                [ownerId](const FileLock& l) { return l.ownerId == ownerId; }),
                locks.end());
        }
        
        for (auto& kv : rangeLocks) {
            auto& locks = kv.second;
            locks.erase(remove_if(locks.begin(), locks.end(),
                [ownerId](const RangeLock& l) { return l.ownerId == ownerId; }),
                locks.end());
        }
        
        if (verbose) cout << "[Lock] Released all locks for owner " << ownerId << "\n";
    }
    
    bool isFileLocked(uint64_t cluster, LfsLockType checkType = LFS_LOCK_EXCLUSIVE) const {
        if (!fileLocks.count(cluster)) return false;
        
        for (const auto& lock : fileLocks.at(cluster)) {
            if (checkType == LFS_LOCK_EXCLUSIVE && lock.type == LFS_LOCK_EXCLUSIVE) return true;
            if (checkType == LFS_LOCK_SHARED) return true;
        }
        return false;
    }
    
    bool canWrite(uint64_t cluster, uint32_t requesterId = 0) {
        lock_guard<mutex> guard(mtx);
        
        if (requesterId == 0) requesterId = currentOwnerId;
        if (!fileLocks.count(cluster)) return true;
        
        for (const auto& lock : fileLocks[cluster]) {
            if (lock.ownerId == requesterId && lock.type == LFS_LOCK_EXCLUSIVE) return true;
            if (lock.ownerId != requesterId && lock.type != LFS_LOCK_NONE) return false;
        }
        return true;
    }
    
    bool canRead(uint64_t cluster, uint32_t requesterId = 0) {
        lock_guard<mutex> guard(mtx);
        
        if (requesterId == 0) requesterId = currentOwnerId;
        if (!fileLocks.count(cluster)) return true;
        
        for (const auto& lock : fileLocks[cluster]) {
            if (lock.ownerId == requesterId) return true;
            if (lock.type == LFS_LOCK_EXCLUSIVE) return false;
        }
        return true;
    }
    
    void displayLocks() const {
        cout << "\n=== Active Locks ===\n";
        
        if (fileLocks.empty() && rangeLocks.empty()) {
            cout << "  No active locks.\n";
            return;
        }
        
        cout << "\nFile Locks:\n";
        for (const auto& kv : fileLocks) {
            for (const auto& lock : kv.second) {
                cout << "  " << lock.fileName << " (cluster " << lock.fileCluster << ")"
                     << " - " << (lock.type == LFS_LOCK_EXCLUSIVE ? "EXCLUSIVE" : "SHARED")
                     << " by PID " << lock.ownerId << "\n";
            }
        }
        
        cout << "\nRange Locks:\n";
        for (const auto& kv : rangeLocks) {
            for (const auto& lock : kv.second) {
                cout << "  Cluster " << lock.fileCluster 
                     << " [" << lock.range.start << "-" << lock.range.end() << "]"
                     << " - " << (lock.type == LFS_LOCK_EXCLUSIVE ? "EXCLUSIVE" : "SHARED")
                     << " by PID " << lock.ownerId << "\n";
            }
        }
    }
    
    size_t countFileLocks() const {
        size_t count = 0;
        for (const auto& kv : fileLocks) count += kv.second.size();
        return count;
    }
    
    size_t countRangeLocks() const {
        size_t count = 0;
        for (const auto& kv : rangeLocks) count += kv.second.size();
        return count;
    }
};

class ScopedFileLock {
private:
    FileLockManager& manager;
    uint64_t cluster;
    bool held;

public:
    ScopedFileLock(FileLockManager& mgr, uint64_t c, const string& name, 
                   LfsLockType type = LFS_LOCK_EXCLUSIVE, uint32_t timeoutMs = 5000)
        : manager(mgr), cluster(c), held(false) {
        LfsLockStatus status = manager.acquireFileLock(c, name, type, timeoutMs);
        held = (status == LFS_LOCK_ACQUIRED);
    }
    
    ~ScopedFileLock() {
        if (held) {
            manager.releaseFileLock(cluster);
        }
    }
    
    bool isHeld() const { return held; }
    
    ScopedFileLock(const ScopedFileLock&) = delete;
    ScopedFileLock& operator=(const ScopedFileLock&) = delete;
};

class ScopedRangeLock {
private:
    FileLockManager& manager;
    uint64_t cluster;
    ByteRange range;
    bool held;

public:
    ScopedRangeLock(FileLockManager& mgr, uint64_t c, const ByteRange& r,
                    LfsLockType type = LFS_LOCK_EXCLUSIVE, uint32_t timeoutMs = 5000)
        : manager(mgr), cluster(c), range(r), held(false) {
        LfsLockStatus status = manager.acquireRangeLock(c, r, type, timeoutMs);
        held = (status == LFS_LOCK_ACQUIRED);
    }
    
    ~ScopedRangeLock() {
        if (held) {
            manager.releaseRangeLock(cluster, range);
        }
    }
    
    bool isHeld() const { return held; }
    
    ScopedRangeLock(const ScopedRangeLock&) = delete;
    ScopedRangeLock& operator=(const ScopedRangeLock&) = delete;
};

#endif
