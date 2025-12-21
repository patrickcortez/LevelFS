/*
 * handler.hpp - Thread Pool and Background Task Handler for LevelFS
 * 
 * Include: #include "utils/handler.hpp"
 * 
 * Features:
 *   - Thread pool for parallel operations
 *   - Background task management with & syntax
 *   - Automatic FS optimization worker
 */

#ifndef HANDLER_HPP
#define HANDLER_HPP

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <future>
#include <queue>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace std;

enum TaskStatus {
    TASK_PENDING = 0,
    TASK_RUNNING = 1,
    TASK_COMPLETED = 2,
    TASK_FAILED = 3
};

struct BackgroundTask {
    uint32_t id;
    string name;
    TaskStatus status;
    chrono::steady_clock::time_point startTime;
    chrono::steady_clock::time_point endTime;
    string result;
    
    BackgroundTask() : id(0), status(TASK_PENDING) {}
};

class ThreadPool {
private:
    vector<thread> workers;
    queue<function<void()>> tasks;
    mutex queueMutex;
    condition_variable condition;
    atomic<bool> stop;
    atomic<size_t> activeTasks;

public:
    ThreadPool(size_t numThreads = 4) : stop(false), activeTasks(0) {
        for (size_t i = 0; i < numThreads; i++) {
            workers.emplace_back([this] {
                while (true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(queueMutex);
                        condition.wait(lock, [this] { 
                            return stop || !tasks.empty(); 
                        });
                        if (stop && tasks.empty()) return;
                        task = move(tasks.front());
                        tasks.pop();
                    }
                    activeTasks++;
                    task();
                    activeTasks--;
                }
            });
        }
    }
    
    ~ThreadPool() {
        {
            unique_lock<mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
    
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> future<typename result_of<F(Args...)>::type> {
        using returnType = typename result_of<F(Args...)>::type;
        
        auto task = make_shared<packaged_task<returnType()>>(
            bind(forward<F>(f), forward<Args>(args)...)
        );
        
        future<returnType> res = task->get_future();
        {
            unique_lock<mutex> lock(queueMutex);
            if (stop) throw runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }
    
    size_t getActiveCount() const { return activeTasks; }
    size_t getPendingCount() { 
        lock_guard<mutex> lock(queueMutex);
        return tasks.size(); 
    }
    size_t getWorkerCount() const { return workers.size(); }
    bool isStopped() const { return stop; }
};

class TaskHandler {
private:
    mutex taskMutex;
    unordered_map<uint32_t, BackgroundTask> tasks;
    vector<thread> taskThreads;
    atomic<uint32_t> nextTaskId;
    atomic<bool> optimizerRunning;
    thread optimizerThread;
    
    function<void()> optimizerCallback;
    uint32_t optimizerIntervalMs;
    
    mutex optimizerMutex;
    condition_variable optimizerCv;

public:
    TaskHandler() : nextTaskId(1), optimizerRunning(false), optimizerIntervalMs(30000) {}
    
    ~TaskHandler() {
        stopOptimizer();
        for (auto& t : taskThreads) {
            if (t.joinable()) t.detach();
        }
    }
    
    uint32_t runBackground(const string& name, function<void()> func) {
        uint32_t taskId = nextTaskId++;
        
        {
            lock_guard<mutex> lock(taskMutex);
            BackgroundTask task;
            task.id = taskId;
            task.name = name;
            task.status = TASK_PENDING;
            task.startTime = chrono::steady_clock::now();
            tasks[taskId] = task;
        }
        
        taskThreads.emplace_back([this, taskId, func, name]() {
            {
                lock_guard<mutex> lock(taskMutex);
                if (tasks.find(taskId) != tasks.end()) {
                    tasks[taskId].status = TASK_RUNNING;
                }
            }
            
            try {
                func();
                lock_guard<mutex> lock(taskMutex);
                if (tasks.find(taskId) != tasks.end()) {
                    tasks[taskId].status = TASK_COMPLETED;
                    tasks[taskId].endTime = chrono::steady_clock::now();
                }
            } catch (const exception& e) {
                lock_guard<mutex> lock(taskMutex);
                if (tasks.find(taskId) != tasks.end()) {
                    tasks[taskId].status = TASK_FAILED;
                    tasks[taskId].result = e.what();
                    tasks[taskId].endTime = chrono::steady_clock::now();
                }
            }
        });
        
        return taskId;
    }
    
    void listTasks() {
        lock_guard<mutex> lock(taskMutex);
        
        if (tasks.empty()) {
            cout << "No background tasks.\n";
            return;
        }
        
        cout << "\n=== Background Tasks ===\n";
        cout << "ID    Status      Name                Duration\n";
        cout << "----  ----------  ------------------  --------\n";
        
        auto now = chrono::steady_clock::now();
        for (const auto& kv : tasks) {
            const BackgroundTask& t = kv.second;
            
            string statusStr;
            switch (t.status) {
                case TASK_PENDING: statusStr = "PENDING"; break;
                case TASK_RUNNING: statusStr = "RUNNING"; break;
                case TASK_COMPLETED: statusStr = "DONE"; break;
                case TASK_FAILED: statusStr = "FAILED"; break;
            }
            
            auto endPoint = (t.status == TASK_COMPLETED || t.status == TASK_FAILED) ? t.endTime : now;
            auto duration = chrono::duration_cast<chrono::seconds>(endPoint - t.startTime).count();
            
            cout << setw(4) << left << t.id << "  "
                 << setw(10) << statusStr << "  "
                 << setw(18) << t.name.substr(0, 18) << "  "
                 << duration << "s\n";
            
            if (t.status == TASK_FAILED && !t.result.empty()) {
                cout << "      Error: " << t.result << "\n";
            }
        }
        cout << "\n";
    }
    
    TaskStatus getTaskStatus(uint32_t taskId) {
        lock_guard<mutex> lock(taskMutex);
        if (tasks.count(taskId)) return tasks[taskId].status;
        return TASK_PENDING;
    }
    
    void cleanupCompleted() {
        lock_guard<mutex> lock(taskMutex);
        for (auto it = tasks.begin(); it != tasks.end();) {
            if (it->second.status == TASK_COMPLETED || it->second.status == TASK_FAILED) {
                it = tasks.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    void startOptimizer(function<void()> callback, uint32_t intervalMs = 30000) {
        if (optimizerRunning) return;
        
        optimizerCallback = callback;
        optimizerIntervalMs = intervalMs;
        optimizerRunning = true;
        
        optimizerThread = thread([this]() {
            while (optimizerRunning) {
                unique_lock<mutex> lock(optimizerMutex);
                optimizerCv.wait_for(lock, chrono::milliseconds(optimizerIntervalMs), [this]() {
                    return !optimizerRunning.load();
                });
                if (!optimizerRunning) break;
                
                try {
                    if (optimizerCallback) optimizerCallback();
                } catch (...) {}
            }
        });
    }
    
    void stopOptimizer() {
        optimizerRunning = false;
        optimizerCv.notify_all();
        if (optimizerThread.joinable()) {
            optimizerThread.join();
        }
    }
    
    bool isOptimizerRunning() const { return optimizerRunning; }
    
    size_t getRunningCount() {
        lock_guard<mutex> lock(taskMutex);
        size_t count = 0;
        for (const auto& kv : tasks) {
            if (kv.second.status == TASK_RUNNING) count++;
        }
        return count;
    }
};

class ParallelExecutor {
private:
    ThreadPool& pool;

public:
    ParallelExecutor(ThreadPool& p) : pool(p) {}
    
    template<typename Iterator, typename Func>
    void parallelFor(Iterator begin, Iterator end, Func func) {
        vector<future<void>> futures;
        
        for (auto it = begin; it != end; ++it) {
            futures.push_back(pool.enqueue([func, it]() { func(*it); }));
        }
        
        for (auto& f : futures) {
            f.wait();
        }
    }
    
    template<typename Func>
    void parallelForRange(size_t start, size_t end, Func func) {
        size_t range = end - start;
        size_t numWorkers = pool.getWorkerCount();
        size_t chunkSize = (range + numWorkers - 1) / numWorkers;
        
        vector<future<void>> futures;
        
        for (size_t i = 0; i < numWorkers && start + i * chunkSize < end; i++) {
            size_t chunkStart = start + i * chunkSize;
            size_t chunkEnd = min(chunkStart + chunkSize, end);
            
            futures.push_back(pool.enqueue([func, chunkStart, chunkEnd]() {
                for (size_t j = chunkStart; j < chunkEnd; j++) {
                    func(j);
                }
            }));
        }
        
        for (auto& f : futures) {
            f.wait();
        }
    }
};

class SpeedTracker {
private:
    mutex mtx;
    vector<double> samples;
    chrono::steady_clock::time_point startTime;
    uint64_t totalBytes;
    
public:
    SpeedTracker() : totalBytes(0) {
        startTime = chrono::steady_clock::now();
    }
    
    void addSample(uint64_t bytes, double durationMs) {
        lock_guard<mutex> lock(mtx);
        if (durationMs > 0) {
            double speed = (bytes / 1024.0) / (durationMs / 1000.0);
            samples.push_back(speed);
        }
        totalBytes += bytes;
    }
    
    double getAvgSpeed() {
        lock_guard<mutex> lock(mtx);
        if (samples.empty()) return 0;
        double sum = 0;
        for (double s : samples) sum += s;
        return sum / samples.size();
    }
    
    double getMinSpeed() {
        lock_guard<mutex> lock(mtx);
        if (samples.empty()) return 0;
        double minVal = samples[0];
        for (double s : samples) if (s < minVal) minVal = s;
        return minVal;
    }
    
    double getMaxSpeed() {
        lock_guard<mutex> lock(mtx);
        if (samples.empty()) return 0;
        double maxVal = samples[0];
        for (double s : samples) if (s > maxVal) maxVal = s;
        return maxVal;
    }
    
    double getTotalTime() {
        auto now = chrono::steady_clock::now();
        return chrono::duration_cast<chrono::milliseconds>(now - startTime).count();
    }
    
    uint64_t getTotalBytes() { return totalBytes; }
    
    static string formatSpeed(double kbps) {
        stringstream ss;
        ss << fixed << setprecision(1);
        if (kbps >= 1000000.0) {
            ss << (kbps / 1000000.0) << " GB/s";
        } else if (kbps >= 1000.0) {
            ss << (kbps / 1000.0) << " MB/s";
        } else {
            ss << kbps << " KB/s";
        }
        return ss.str();
    }
};

class ProgressBar {
private:
    uint64_t total;
    atomic<uint64_t> current;
    int barWidth;
    string label;
    bool finished;
    
public:
    ProgressBar(uint64_t totalSize, const string& lbl = "", int width = 40) 
        : total(totalSize), current(0), barWidth(width), label(lbl), finished(false) {}
    
    void update(uint64_t processed) {
        current = processed;
    }
    
    void add(uint64_t delta) {
        current += delta;
    }
    
    void display(const SpeedTracker& tracker) {
        if (finished) return;
        
        double percent = (total > 0) ? (100.0 * current / total) : 100.0;
        int filled = (int)(barWidth * current / max(total, (uint64_t)1));
        
        cout << "\r";
        if (!label.empty()) cout << label << " ";
        cout << "[";
        for (int i = 0; i < barWidth; i++) {
            if (i < filled) cout << "=";
            else if (i == filled) cout << ">";
            else cout << " ";
        }
        cout << "] " << fixed << setprecision(1) << percent << "%";
        cout << flush;
    }
    
    void displayWithSpeed(SpeedTracker& tracker) {
        if (finished) return;
        
        double percent = (total > 0) ? (100.0 * current / total) : 100.0;
        int filled = (int)(barWidth * current / max(total, (uint64_t)1));
        
        cout << "\r";
        if (!label.empty()) cout << label << " ";
        cout << "[";
        for (int i = 0; i < barWidth; i++) {
            if (i < filled) cout << "=";
            else if (i == filled) cout << ">";
            else cout << " ";
        }
        cout << "] " << fixed << setprecision(1) << percent << "%\n";
        
        cout << "Speed - Avg: " << SpeedTracker::formatSpeed(tracker.getAvgSpeed())
             << " | Min: " << SpeedTracker::formatSpeed(tracker.getMinSpeed())
             << " | Max: " << SpeedTracker::formatSpeed(tracker.getMaxSpeed())
             << "\033[K";
        cout << "\033[1A";
        cout << flush;
    }
    
    void finish(SpeedTracker& tracker) {
        if (finished) return;
        finished = true;
        
        current = total;
        cout << "\r";
        if (!label.empty()) cout << label << " ";
        cout << "[";
        for (int i = 0; i < barWidth; i++) cout << "=";
        cout << "] 100.0%\n";
        
        cout << "Speed - Avg: " << SpeedTracker::formatSpeed(tracker.getAvgSpeed())
             << " | Min: " << SpeedTracker::formatSpeed(tracker.getMinSpeed())
             << " | Max: " << SpeedTracker::formatSpeed(tracker.getMaxSpeed()) << "\n";
    }
    
    uint64_t getCurrent() const { return current; }
    uint64_t getTotal() const { return total; }
    bool isFinished() const { return finished; }
};

class TransferProgress {
public:
    ProgressBar bar;
    SpeedTracker tracker;
    chrono::steady_clock::time_point lastUpdate;
    
    TransferProgress(uint64_t totalSize, const string& label = "")
        : bar(totalSize, label) {
        lastUpdate = chrono::steady_clock::now();
    }
    
    void update(uint64_t processed, uint64_t chunkBytes) {
        auto now = chrono::steady_clock::now();
        double elapsed = chrono::duration_cast<chrono::milliseconds>(now - lastUpdate).count();
        
        tracker.addSample(chunkBytes, elapsed);
        bar.update(processed);
        bar.displayWithSpeed(tracker);
        
        lastUpdate = now;
    }
    
    void finish() {
        bar.finish(tracker);
    }
};

#endif

