/*
 * scheduler.hpp - Process Scheduler and Manager for LevelFS
 * 
 * Include: #include "scheduler.hpp"
 * 
 * Features:
 *   - .prc file parsing and execution
 *   - Process lifecycle management (start, kill, list)
 *   - OnReboot process persistence
 *   - Integration with LevelFS file system
 */

#ifndef SCHEDULER_HPP
#define SCHEDULER_HPP

#include <windows.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>

using namespace std;

struct ProcessDefinition {
    string name;
    string path;
    vector<string> args;
    bool onReboot;
    
    ProcessDefinition() : onReboot(false) {}
};

struct ManagedProcess {
    uint32_t pid;
    string name;
    string path;
    string args;
    HANDLE processHandle;
    HANDLE threadHandle;
    DWORD systemPid;
    chrono::steady_clock::time_point startTime;
    bool isRunning;
    bool onReboot;
    
    ManagedProcess() : pid(0), processHandle(NULL), threadHandle(NULL), 
                       systemPid(0), isRunning(false), onReboot(false) {}
};

class PrcParser {
public:
    static ProcessDefinition parse(const string& content) {
        ProcessDefinition def;
        
        string line;
        istringstream stream(content);
        
        while (getline(stream, line)) {
            line = trim(line);
            if (line.empty()) continue;
            
            if (line.back() == ':' && line.front() == '[') {
                size_t end = line.find(']');
                if (end != string::npos) {
                    def.name = line.substr(1, end - 1);
                }
            }
            else if (startsWith(line, "Path:")) {
                def.path = extractQuotedValue(line.substr(5));
            }
            else if (startsWith(line, "Args:")) {
                string argsStr = line.substr(5);
                def.args = parseArgsList(argsStr);
            }
            else if (startsWith(line, "onReboot:")) {
                string val = trim(line.substr(9));
                if (val.back() == ';') val.pop_back();
                val = trim(val);
                def.onReboot = (val == "true" || val == "1" || val == "yes");
            }
        }
        
        return def;
    }
    
    static string serialize(const ProcessDefinition& def) {
        stringstream ss;
        ss << "[" << def.name << "]:\n";
        ss << "    Path: \"" << def.path << "\";\n";
        
        ss << "    Args: ";
        for (size_t i = 0; i < def.args.size(); i++) {
            if (i > 0) ss << ", ";
            ss << "\"" << def.args[i] << "\"";
        }
        ss << ";\n";
        
        ss << "    onReboot: " << (def.onReboot ? "true" : "false") << ";\n";
        ss << ".\n";
        
        return ss.str();
    }
    
private:
    static string trim(const string& s) {
        size_t start = 0, end = s.length();
        while (start < end && isspace(s[start])) start++;
        while (end > start && isspace(s[end - 1])) end--;
        return s.substr(start, end - start);
    }
    
    static bool startsWith(const string& s, const string& prefix) {
        if (s.length() < prefix.length()) return false;
        return s.compare(0, prefix.length(), prefix) == 0;
    }
    
    static string extractQuotedValue(const string& s) {
        size_t first = s.find('"');
        if (first == string::npos) return trim(s);
        size_t last = s.find('"', first + 1);
        if (last == string::npos) return s.substr(first + 1);
        return s.substr(first + 1, last - first - 1);
    }
    
    static vector<string> parseArgsList(const string& s) {
        vector<string> args;
        bool inQuote = false;
        string current;
        
        for (size_t i = 0; i < s.length(); i++) {
            char c = s[i];
            
            if (c == '"') {
                if (inQuote) {
                    if (!current.empty()) {
                        args.push_back(current);
                        current.clear();
                    }
                    inQuote = false;
                } else {
                    inQuote = true;
                }
            }
            else if (c == ',' && !inQuote) {
                continue;
            }
            else if (c == ';' && !inQuote) {
                break;
            }
            else if (inQuote) {
                current += c;
            }
        }
        
        if (!current.empty()) {
            args.push_back(current);
        }
        
        return args;
    }
};

class ProcessScheduler {
private:
    mutex processMutex;
    unordered_map<uint32_t, ManagedProcess> processes;
    atomic<uint32_t> nextPid;
    string startupFilePath;
    
    function<string(const string&)> readFileCallback;
    function<void(const string&, const string&)> writeFileCallback;
    function<bool(const string&)> fileExistsCallback;
    function<string(const string&)> extractFileCallback;

public:
    ProcessScheduler() : nextPid(1) {}
    
    ~ProcessScheduler() {
        lock_guard<mutex> lock(processMutex);
        for (auto& kv : processes) {
            if (kv.second.isRunning && kv.second.processHandle != NULL) {
                TerminateProcess(kv.second.processHandle, 0);
                CloseHandle(kv.second.processHandle);
                CloseHandle(kv.second.threadHandle);
            }
        }
    }
    
    void setFileCallbacks(
        function<string(const string&)> readCb,
        function<void(const string&, const string&)> writeCb,
        function<bool(const string&)> existsCb,
        function<string(const string&)> extractCb = nullptr
    ) {
        readFileCallback = readCb;
        writeFileCallback = writeCb;
        fileExistsCallback = existsCb;
        extractFileCallback = extractCb;
    }
    
    void setStartupFile(const string& path) {
        startupFilePath = path;
    }
    
    uint32_t startFromPrcFile(const string& prcContent, const string& sourceName = "") {
        ProcessDefinition def = PrcParser::parse(prcContent);
        
        if (def.path.empty()) {
            cout << "Error: No executable path specified in .prc file\n";
            return 0;
        }
        
        if (def.name.empty()) {
            def.name = sourceName.empty() ? "Process" : sourceName;
        }
        
        return startProcess(def);
    }
    
    uint32_t startProcess(const ProcessDefinition& def) {
        uint32_t pid = nextPid++;
        
        string argsStr;
        for (size_t i = 0; i < def.args.size(); i++) {
            if (i > 0) argsStr += " ";
            if (def.args[i].find(' ') != string::npos) {
                argsStr += "\"" + def.args[i] + "\"";
            } else {
                argsStr += def.args[i];
            }
        }
        
        string execPath = def.path;
        string tempFile;
        
        bool isLfsPath = (def.path.length() > 0 && def.path[0] == '/') ||
                         (def.path.find(':') != string::npos && def.path.find(":\\") == string::npos);
        
        if (isLfsPath && extractFileCallback) {
            tempFile = extractFileCallback(def.path);
            if (tempFile.empty()) {
                cout << "Failed to extract LevelFS file: " << def.path << "\n";
                return 0;
            }
            execPath = tempFile;
            cout << "Extracted to: " << tempFile << "\n";
        }
        
        string cmdLine = "\"" + execPath + "\"";
        if (!argsStr.empty()) {
            cmdLine += " " + argsStr;
        }
        
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        
        char cmdBuf[4096];
        strncpy(cmdBuf, cmdLine.c_str(), sizeof(cmdBuf) - 1);
        cmdBuf[sizeof(cmdBuf) - 1] = '\0';
        
        if (!CreateProcessA(NULL, cmdBuf, NULL, NULL, FALSE, 
                           CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
            cout << "Failed to start process. Error: " << GetLastError() << "\n";
            return 0;
        }
        
        ManagedProcess proc;
        proc.pid = pid;
        proc.name = def.name;
        proc.path = def.path;
        proc.args = argsStr;
        proc.processHandle = pi.hProcess;
        proc.threadHandle = pi.hThread;
        proc.systemPid = pi.dwProcessId;
        proc.startTime = chrono::steady_clock::now();
        proc.isRunning = true;
        proc.onReboot = def.onReboot;
        
        {
            lock_guard<mutex> lock(processMutex);
            processes[pid] = proc;
        }
        
        thread([this, pid]() {
            monitorProcess(pid);
        }).detach();
        
        cout << "Started process '" << def.name << "' (PID: " << pid 
             << ", System PID: " << pi.dwProcessId << ")\n";
        
        if (def.onReboot) {
            saveStartupProcess(def);
        }
        
        return pid;
    }
    
    bool killProcess(uint32_t pid) {
        lock_guard<mutex> lock(processMutex);
        
        auto it = processes.find(pid);
        if (it == processes.end()) {
            cout << "Process not found: PID " << pid << "\n";
            return false;
        }
        
        ManagedProcess& proc = it->second;
        
        if (!proc.isRunning) {
            cout << "Process already terminated: " << proc.name << "\n";
            return false;
        }
        
        if (proc.processHandle != NULL) {
            if (TerminateProcess(proc.processHandle, 0)) {
                proc.isRunning = false;
                CloseHandle(proc.processHandle);
                CloseHandle(proc.threadHandle);
                proc.processHandle = NULL;
                proc.threadHandle = NULL;
                
                cout << "Killed process '" << proc.name << "' (PID: " << pid << ")\n";
                return true;
            } else {
                cout << "Failed to kill process. Error: " << GetLastError() << "\n";
                return false;
            }
        }
        
        return false;
    }
    
    void listProcesses() {
        lock_guard<mutex> lock(processMutex);
        
        updateProcessStatuses();
        
        size_t runningCount = 0;
        for (const auto& kv : processes) {
            if (kv.second.isRunning) runningCount++;
        }
        
        if (processes.empty()) {
            cout << "No managed processes.\n";
            return;
        }
        
        cout << "\n=== Managed Processes ===\n";
        cout << "PID   SPID      Status    Name                  Runtime\n";
        cout << "----  --------  --------  --------------------  --------\n";
        
        auto now = chrono::steady_clock::now();
        
        for (const auto& kv : processes) {
            const ManagedProcess& p = kv.second;
            
            string statusStr = p.isRunning ? "RUNNING" : "STOPPED";
            if (p.onReboot && p.isRunning) statusStr += "*";
            
            auto duration = chrono::duration_cast<chrono::seconds>(now - p.startTime).count();
            string durationStr = formatDuration(duration);
            
            cout << setw(4) << left << p.pid << "  "
                 << setw(8) << p.systemPid << "  "
                 << setw(8) << statusStr << "  "
                 << setw(20) << p.name.substr(0, 20) << "  "
                 << durationStr << "\n";
        }
        
        cout << "\nTotal: " << processes.size() << " | Running: " << runningCount << "\n";
        cout << "(* = onReboot enabled)\n\n";
    }
    
    void listProcessesShort() {
        lock_guard<mutex> lock(processMutex);
        updateProcessStatuses();
        
        if (processes.empty()) {
            cout << "No managed processes.\n";
            return;
        }
        
        cout << "PID   Name                  Status\n";
        cout << "----  --------------------  --------\n";
        
        for (const auto& kv : processes) {
            const ManagedProcess& p = kv.second;
            cout << setw(4) << left << p.pid << "  "
                 << setw(20) << p.name.substr(0, 20) << "  "
                 << (p.isRunning ? "RUNNING" : "STOPPED") << "\n";
        }
    }
    
    void cleanupTerminated() {
        lock_guard<mutex> lock(processMutex);
        updateProcessStatuses();
        
        size_t removed = 0;
        for (auto it = processes.begin(); it != processes.end();) {
            if (!it->second.isRunning) {
                if (it->second.processHandle != NULL) {
                    CloseHandle(it->second.processHandle);
                    CloseHandle(it->second.threadHandle);
                }
                it = processes.erase(it);
                removed++;
            } else {
                ++it;
            }
        }
        
        if (removed > 0) {
            cout << "Cleaned up " << removed << " terminated process(es).\n";
        }
    }
    
    void loadStartupProcesses() {
        if (startupFilePath.empty() || !fileExistsCallback) return;
        
        if (!fileExistsCallback(startupFilePath)) return;
        
        string content = readFileCallback(startupFilePath);
        if (content.empty()) return;
        
        cout << "Loading startup processes...\n";
        
        istringstream stream(content);
        string line;
        string currentBlock;
        
        while (getline(stream, line)) {
            currentBlock += line + "\n";
            
            if (!line.empty() && line.find('.') == 0 && line.length() == 1) {
                if (!currentBlock.empty()) {
                    ProcessDefinition def = PrcParser::parse(currentBlock);
                    if (!def.path.empty()) {
                        cout << "  Starting: " << def.name << "\n";
                        startProcess(def);
                    }
                }
                currentBlock.clear();
            }
        }
        
        if (!currentBlock.empty()) {
            ProcessDefinition def = PrcParser::parse(currentBlock);
            if (!def.path.empty()) {
                cout << "  Starting: " << def.name << "\n";
                startProcess(def);
            }
        }
    }
    
    size_t getRunningCount() {
        lock_guard<mutex> lock(processMutex);
        updateProcessStatuses();
        
        size_t count = 0;
        for (const auto& kv : processes) {
            if (kv.second.isRunning) count++;
        }
        return count;
    }
    
    bool isProcessRunning(uint32_t pid) {
        lock_guard<mutex> lock(processMutex);
        auto it = processes.find(pid);
        if (it == processes.end()) return false;
        
        updateSingleProcessStatus(it->second);
        return it->second.isRunning;
    }
    
    ManagedProcess* getProcess(uint32_t pid) {
        lock_guard<mutex> lock(processMutex);
        auto it = processes.find(pid);
        if (it == processes.end()) return nullptr;
        return &it->second;
    }

private:
    void monitorProcess(uint32_t pid) {
        HANDLE hProcess = NULL;
        
        {
            lock_guard<mutex> lock(processMutex);
            auto it = processes.find(pid);
            if (it == processes.end()) return;
            hProcess = it->second.processHandle;
        }
        
        if (hProcess == NULL) return;
        
        WaitForSingleObject(hProcess, INFINITE);
        
        {
            lock_guard<mutex> lock(processMutex);
            auto it = processes.find(pid);
            if (it != processes.end()) {
                it->second.isRunning = false;
            }
        }
    }
    
    void updateProcessStatuses() {
        for (auto& kv : processes) {
            updateSingleProcessStatus(kv.second);
        }
    }
    
    void updateSingleProcessStatus(ManagedProcess& proc) {
        if (!proc.isRunning || proc.processHandle == NULL) return;
        
        DWORD exitCode;
        if (GetExitCodeProcess(proc.processHandle, &exitCode)) {
            if (exitCode != STILL_ACTIVE) {
                proc.isRunning = false;
            }
        }
    }
    
    void saveStartupProcess(const ProcessDefinition& def) {
        if (startupFilePath.empty() || !writeFileCallback) return;
        
        string existing;
        if (fileExistsCallback && fileExistsCallback(startupFilePath)) {
            existing = readFileCallback(startupFilePath);
        }
        
        string serialized = PrcParser::serialize(def);
        
        if (existing.find(def.name) != string::npos) {
            return;
        }
        
        string newContent = existing + serialized;
        writeFileCallback(startupFilePath, newContent);
    }
    
    static string formatDuration(long seconds) {
        if (seconds < 60) return to_string(seconds) + "s";
        if (seconds < 3600) return to_string(seconds / 60) + "m " + to_string(seconds % 60) + "s";
        long hours = seconds / 3600;
        long mins = (seconds % 3600) / 60;
        return to_string(hours) + "h " + to_string(mins) + "m";
    }
};

#endif
