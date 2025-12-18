/*
 * lstream.hpp - LevelFS Independent Stream Buffer
 * 
 * Compile: Include with #include "lstream.hpp", link with lstream.cpp
 * 
 * Features:
 *   - Fully self-contained stream buffer (no iostream dependency)
 *   - Drop-in replacement for cout/cin
 *   - Buffer capture and redirect capabilities
 *   - Scrollback history support
 */

#ifndef LSTREAM_HPP
#define LSTREAM_HPP

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ios>

using namespace std;

class LStreamBuf {
public:
    using OutputCallback = function<void(const string&)>;

private:
    string buffer;
    deque<string> history;
    size_t maxHistory;
    mutex mtx;
    
    OutputCallback outputCallback;
    FILE* mirrorFile;
    bool mirrorEnabled;
    bool bufferEnabled;
    bool directConsole;
    
    void flushLineUnsafe();
    void flushPartialUnsafe();

public:
    LStreamBuf(size_t historySize = 1000);
    
    void write(char c);
    void write(const char* s, size_t n);
    void write(const string& s);
    void flush();
    
    void setOutputCallback(OutputCallback cb);
    void setMirror(FILE* file);
    void enableMirror(bool enable);
    void enableBuffer(bool enable);
    void enableDirectConsole(bool enable);
    void setMaxHistory(size_t max);
    
    const deque<string>& getHistory() const;
    void clearHistory();
    string getHistoryAsString() const;
    size_t getHistorySize() const;
};

class LInputBuf {
private:
    string inputBuffer;
    size_t readPos;
    mutex mtx;
    function<string()> inputCallback;

public:
    LInputBuf();
    
    int read();
    string readLine();
    bool hasData() const;
    
    void setInputCallback(function<string()> cb);
    void feedInput(const string& text);
    void clearBuffer();
};

struct LSetW { int width; };

class LOutputStream {
private:
    LStreamBuf buf;
    int fieldWidth;
    bool leftAlign;
    int numBase;

    string applyFormat(const string& s);

public:
    static LSetW width(int w) { return LSetW{w}; }
    
    LOutputStream(size_t historySize = 1000);
    
    LOutputStream& operator<<(const char* s);
    LOutputStream& operator<<(const string& s);
    LOutputStream& operator<<(char c);
    LOutputStream& operator<<(int val);
    LOutputStream& operator<<(unsigned int val);
    LOutputStream& operator<<(long val);
    LOutputStream& operator<<(unsigned long val);
    LOutputStream& operator<<(long long val);
    LOutputStream& operator<<(unsigned long long val);
    LOutputStream& operator<<(double val);
    LOutputStream& operator<<(float val);
    LOutputStream& operator<<(bool val);
    LOutputStream& operator<<(const void* ptr);
    LOutputStream& operator<<(LOutputStream& (*manip)(LOutputStream&));
    LOutputStream& operator<<(LSetW sw);
    
    void setWidth(int w);
    void setLeftAlign(bool align);
    void setBase(int base);
    
    void write(const char* data, size_t len);
    
    void setOutputCallback(LStreamBuf::OutputCallback cb);
    void enableDirectConsole(bool enable);
    void enableBuffer(bool enable);
    void setMaxHistory(size_t max);
    
    const deque<string>& getHistory() const;
    void clearHistory();
    string getHistoryAsString() const;
    size_t getHistorySize() const;
    
    void flush();
    LStreamBuf* rdbuf();
    
    inline LOutputStream& operator<<(decltype(std::setw(0)) sw) {
        fieldWidth = sw._M_n;
        return *this;
    }
    
    inline LOutputStream& operator<<(ios_base& (*manip)(ios_base&)) {
        if (manip == std::left) leftAlign = true;
        else if (manip == std::right) leftAlign = false;
        else if (manip == std::hex) numBase = 16;
        else if (manip == std::dec) numBase = 10;
        return *this;
    }
};

class LInputStream {
private:
    LInputBuf buf;
    
public:
    LInputStream();
    
    LInputStream& operator>>(string& s);
    LInputStream& operator>>(int& val);
    LInputStream& operator>>(double& val);
    LInputStream& operator>>(char& c);
    
    bool getline(string& line);
    
    void setInputCallback(function<string()> cb);
    void feedInput(const string& text);
    void clearInput();
    bool hasData() const;
};

class LStream {
private:
    LOutputStream out;
    LInputStream in;
    
public:
    LStream(size_t historySize = 1000);
    
    LOutputStream& output();
    LInputStream& input();
    
    void setOutputCallback(LStreamBuf::OutputCallback cb);
    void setInputCallback(function<string()> cb);
    void enableDirectConsole(bool enable);
    
    const deque<string>& getHistory() const;
    void clearHistory();
    void feedInput(const string& text);
};

LOutputStream& endl(LOutputStream& os);
LOutputStream& flush(LOutputStream& os);

extern LStream lstream;

#define lout lstream.output()
#define lin lstream.input()

#endif
