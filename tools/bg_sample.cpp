/*
 * bg_sample.cpp - Sample Background Process for LevelFS
 * 
 * Compile: g++ -std=c++17 -static tools/bg_sample.cpp -o tools/bg_sample.exe
 * 
 * A simple background process that logs timestamps to a file every 5 seconds.
 * Runs until manually terminated.
 */

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <ctime>
#include <string>
#include <iomanip>
#include <sstream>

using namespace std;

string getTimestamp() {
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm* tm_info = localtime(&t);
    
    stringstream ss;
    ss << put_time(tm_info, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

int main(int argc, char* argv[]) {
    string logPath = "bg_sample.log";
    int intervalSec = 5;
    
    if (argc > 1) {
        logPath = argv[1];
    }
    if (argc > 2) {
        intervalSec = atoi(argv[2]);
        if (intervalSec < 1) intervalSec = 5;
    }
    
    cout << "=================================\n";
    cout << "  LevelFS Background Sample\n";
    cout << "=================================\n";
    cout << "Log file: " << logPath << "\n";
    cout << "Interval: " << intervalSec << " seconds\n";
    cout << "Press Ctrl+C to stop.\n\n";
    
    int counter = 0;
    
    while (true) {
        counter++;
        string timestamp = getTimestamp();
        string message = "[" + timestamp + "] Heartbeat #" + to_string(counter);
        
        cout << message << "\n";
        
        ofstream logFile(logPath, ios::app);
        if (logFile.is_open()) {
            logFile << message << "\n";
            logFile.close();
        }
        
        this_thread::sleep_for(chrono::seconds(intervalSec));
    }
    
    return 0;
}
