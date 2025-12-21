/*
 * handler.cpp - Background Task Handler Implementation for LevelFS
 * 
 * Compile: Include via mount.cpp build command
 *   g++ -std=c++17 -static -static-libgcc -static-libstdc++ src/mount.cpp src/lstream.cpp -o bin/mount.exe
 */

#include "handler.hpp"
#include <algorithm>
#include <sstream>

class OptimizerConfig {
public:
    uint32_t intervalMs = 30000;
    uint32_t maxClustersPerPass = 100;
    bool aggressiveMode = false;
    bool verboseLogging = false;
    
    static OptimizerConfig& getInstance() {
        static OptimizerConfig instance;
        return instance;
    }
};

class FSOptimizer {
private:
    ThreadPool& pool;
    mutex optimizerMutex;
    atomic<bool> isRunning;
    atomic<uint64_t> totalMoves;
    atomic<uint64_t> totalScans;
    
public:
    FSOptimizer(ThreadPool& p) : pool(p), isRunning(false), totalMoves(0), totalScans(0) {}
    
    void runParallelScan(size_t startCluster, size_t endCluster, 
                         function<bool(size_t)> scanFunc) {
        if (startCluster >= endCluster) return;
        
        size_t range = endCluster - startCluster;
        size_t numWorkers = pool.getWorkerCount();
        size_t chunkSize = (range + numWorkers - 1) / numWorkers;
        
        vector<future<void>> futures;
        
        for (size_t i = 0; i < numWorkers && startCluster + i * chunkSize < endCluster; i++) {
            size_t chunkStart = startCluster + i * chunkSize;
            size_t chunkEnd = min(chunkStart + chunkSize, endCluster);
            
            futures.push_back(pool.enqueue([this, scanFunc, chunkStart, chunkEnd]() {
                for (size_t c = chunkStart; c < chunkEnd; c++) {
                    scanFunc(c);
                    totalScans++;
                }
            }));
        }
        
        for (auto& f : futures) {
            f.wait();
        }
    }
    
    template<typename T>
    void parallelProcess(vector<T>& items, function<void(T&)> processor) {
        if (items.empty()) return;
        
        size_t numWorkers = pool.getWorkerCount();
        size_t chunkSize = (items.size() + numWorkers - 1) / numWorkers;
        
        vector<future<void>> futures;
        
        for (size_t i = 0; i < numWorkers && i * chunkSize < items.size(); i++) {
            size_t start = i * chunkSize;
            size_t end = min(start + chunkSize, items.size());
            
            futures.push_back(pool.enqueue([&items, processor, start, end]() {
                for (size_t j = start; j < end; j++) {
                    processor(items[j]);
                }
            }));
        }
        
        for (auto& f : futures) {
            f.wait();
        }
    }
    
    uint64_t getTotalMoves() const { return totalMoves; }
    uint64_t getTotalScans() const { return totalScans; }
    void resetStats() { totalMoves = 0; totalScans = 0; }
};

class CommandParser {
public:
    struct ParsedCommand {
        string command;
        vector<string> args;
        bool runInBackground;
        string rawLine;
    };
    
    static ParsedCommand parse(const string& input) {
        ParsedCommand result;
        result.rawLine = input;
        result.runInBackground = false;
        
        string trimmed = input;
        while (!trimmed.empty() && isspace(trimmed.back())) {
            trimmed.pop_back();
        }
        while (!trimmed.empty() && isspace(trimmed.front())) {
            trimmed.erase(0, 1);
        }
        
        if (!trimmed.empty() && trimmed.back() == '&') {
            result.runInBackground = true;
            trimmed.pop_back();
            while (!trimmed.empty() && isspace(trimmed.back())) {
                trimmed.pop_back();
            }
        }
        
        stringstream ss(trimmed);
        ss >> result.command;
        
        string arg;
        while (ss >> arg) {
            result.args.push_back(arg);
        }
        
        return result;
    }
    
    static string argsToString(const vector<string>& args) {
        string result;
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) result += " ";
            result += args[i];
        }
        return result;
    }
};

class TaskRegistry {
private:
    mutex registryMutex;
    unordered_map<string, function<void(const vector<string>&)>> handlers;
    
public:
    void registerHandler(const string& command, function<void(const vector<string>&)> handler) {
        lock_guard<mutex> lock(registryMutex);
        handlers[command] = handler;
    }
    
    bool hasHandler(const string& command) {
        lock_guard<mutex> lock(registryMutex);
        return handlers.count(command) > 0;
    }
    
    function<void(const vector<string>&)> getHandler(const string& command) {
        lock_guard<mutex> lock(registryMutex);
        if (handlers.count(command)) {
            return handlers[command];
        }
        return nullptr;
    }
    
    void executeCommand(const string& command, const vector<string>& args) {
        auto handler = getHandler(command);
        if (handler) {
            handler(args);
        }
    }
};
