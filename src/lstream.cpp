/*
 * lstream.cpp - LevelFS Independent Stream Buffer Implementation
 * 
 * Compile: g++ -c lstream.cpp -o lstream.o
 *          Link with: g++ main.cpp lstream.o -o main.exe
 */

#include "lstream.hpp"
#include <cstdlib>

LStreamBuf::LStreamBuf(size_t historySize) 
    : maxHistory(historySize), mirrorFile(nullptr), 
      mirrorEnabled(false), bufferEnabled(true), directConsole(true) {
    buffer.reserve(256);
}

void LStreamBuf::write(char c) {
    lock_guard<mutex> guard(mtx);
    
    if (c == '\n') {
        flushLineUnsafe();
    } else {
        buffer += c;
    }
}

void LStreamBuf::write(const char* s, size_t n) {
    lock_guard<mutex> guard(mtx);
    
    buffer.reserve(buffer.size() + n);
    
    const char* start = s;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\n') {
            buffer.append(start, s + i - start);
            flushLineUnsafe();
            start = s + i + 1;
        }
    }
    
    if (start < s + n) {
        buffer.append(start, s + n - start);
    }
}

void LStreamBuf::write(const string& s) {
    write(s.c_str(), s.size());
}

void LStreamBuf::flush() {
    lock_guard<mutex> guard(mtx);
    if (!buffer.empty()) {
        flushPartialUnsafe();
    }
}

void LStreamBuf::flushLineUnsafe() {
    string line = move(buffer);
    buffer.clear();
    buffer.reserve(256);
    
    if (bufferEnabled) {
        history.push_back(line);
        if (history.size() > maxHistory) {
            history.pop_front();
        }
    }
    
    if (directConsole) {
        fputs(line.c_str(), stdout);
        fputs("\n", stdout);
        fflush(stdout);
    }
    
    if (outputCallback) {
        outputCallback(line + "\n");
    }
    
    if (mirrorEnabled && mirrorFile && mirrorFile != stdout) {
        fputs(line.c_str(), mirrorFile);
        fputs("\n", mirrorFile);
        fflush(mirrorFile);
    }
}

void LStreamBuf::flushPartialUnsafe() {
    string partial = move(buffer);
    buffer.clear();
    buffer.reserve(256);
    
    if (directConsole) {
        fputs(partial.c_str(), stdout);
        fflush(stdout);
    }
    
    if (outputCallback) {
        outputCallback(partial);
    }
    
    if (mirrorEnabled && mirrorFile && mirrorFile != stdout) {
        fputs(partial.c_str(), mirrorFile);
        fflush(mirrorFile);
    }
}

void LStreamBuf::setOutputCallback(OutputCallback cb) { 
    lock_guard<mutex> guard(mtx);
    outputCallback = cb; 
}

void LStreamBuf::setMirror(FILE* file) { 
    lock_guard<mutex> guard(mtx);
    mirrorFile = file; 
}

void LStreamBuf::enableMirror(bool enable) { 
    lock_guard<mutex> guard(mtx);
    mirrorEnabled = enable; 
}

void LStreamBuf::enableBuffer(bool enable) { 
    lock_guard<mutex> guard(mtx);
    bufferEnabled = enable; 
}

void LStreamBuf::enableDirectConsole(bool enable) { 
    lock_guard<mutex> guard(mtx);
    directConsole = enable; 
}

void LStreamBuf::setMaxHistory(size_t max) { 
    lock_guard<mutex> guard(mtx);
    maxHistory = max;
    while (history.size() > maxHistory) {
        history.pop_front();
    }
}

const deque<string>& LStreamBuf::getHistory() const { 
    return history; 
}

void LStreamBuf::clearHistory() {
    lock_guard<mutex> guard(mtx);
    history.clear();
}

string LStreamBuf::getHistoryAsString() const {
    string result;
    for (const auto& line : history) {
        result += line + "\n";
    }
    return result;
}

size_t LStreamBuf::getHistorySize() const { 
    return history.size(); 
}

LInputBuf::LInputBuf() : readPos(0) {}

int LInputBuf::read() {
    lock_guard<mutex> guard(mtx);
    
    if (readPos >= inputBuffer.size()) {
        if (inputCallback) {
            inputBuffer = inputCallback();
            readPos = 0;
        }
        
        if (inputBuffer.empty() || readPos >= inputBuffer.size()) {
            return -1;
        }
    }
    
    return static_cast<unsigned char>(inputBuffer[readPos++]);
}

string LInputBuf::readLine() {
    lock_guard<mutex> guard(mtx);
    
    if (readPos >= inputBuffer.size()) {
        if (inputCallback) {
            inputBuffer = inputCallback();
            readPos = 0;
        }
    }
    
    string line;
    while (readPos < inputBuffer.size()) {
        char c = inputBuffer[readPos++];
        if (c == '\n') break;
        line += c;
    }
    
    return line;
}

bool LInputBuf::hasData() const {
    return readPos < inputBuffer.size();
}

void LInputBuf::setInputCallback(function<string()> cb) { 
    lock_guard<mutex> guard(mtx);
    inputCallback = cb; 
}

void LInputBuf::feedInput(const string& text) {
    lock_guard<mutex> guard(mtx);
    inputBuffer += text;
}

void LInputBuf::clearBuffer() {
    lock_guard<mutex> guard(mtx);
    inputBuffer.clear();
    readPos = 0;
}

LOutputStream::LOutputStream(size_t historySize) : buf(historySize), fieldWidth(0), leftAlign(false), numBase(10) {}

string LOutputStream::applyFormat(const string& s) {
    int width = fieldWidth;
    fieldWidth = 0;
    
    if (width <= 0) {
        return s;
    }
    
    size_t contentLen = 0;
    for (char c : s) {
        if (c != '\n' && c != '\r' && c != '\t') contentLen++;
    }
    
    if (contentLen >= static_cast<size_t>(width)) {
        return s;
    }
    
    size_t padding = width - contentLen;
    string result;
    result.reserve(s.length() + padding);
    
    if (leftAlign) {
        result = s;
        size_t nl = result.find('\n');
        if (nl != string::npos) {
            result.insert(nl, padding, ' ');
        } else {
            result.append(padding, ' ');
        }
    } else {
        result.append(padding, ' ');
        result += s;
    }
    
    return result;
}

LOutputStream& LOutputStream::operator<<(const char* s) {
    if (fieldWidth > 0) {
        buf.write(applyFormat(s));
    } else {
        buf.write(s, strlen(s));
    }
    return *this;
}

LOutputStream& LOutputStream::operator<<(const string& s) {
    if (fieldWidth > 0) {
        buf.write(applyFormat(s));
    } else {
        buf.write(s);
    }
    return *this;
}

LOutputStream& LOutputStream::operator<<(char c) {
    buf.write(c);
    return *this;
}

LOutputStream& LOutputStream::operator<<(int val) {
    char tmp[32];
    if (numBase == 16) snprintf(tmp, sizeof(tmp), "%x", val);
    else snprintf(tmp, sizeof(tmp), "%d", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(unsigned int val) {
    char tmp[32];
    if (numBase == 16) snprintf(tmp, sizeof(tmp), "%x", val);
    else snprintf(tmp, sizeof(tmp), "%u", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(long val) {
    char tmp[32];
    if (numBase == 16) snprintf(tmp, sizeof(tmp), "%lx", val);
    else snprintf(tmp, sizeof(tmp), "%ld", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(unsigned long val) {
    char tmp[32];
    if (numBase == 16) snprintf(tmp, sizeof(tmp), "%lx", val);
    else snprintf(tmp, sizeof(tmp), "%lu", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(long long val) {
    char tmp[32];
    if (numBase == 16) snprintf(tmp, sizeof(tmp), "%llx", val);
    else snprintf(tmp, sizeof(tmp), "%lld", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(unsigned long long val) {
    char tmp[32];
    if (numBase == 16) snprintf(tmp, sizeof(tmp), "%llx", val);
    else snprintf(tmp, sizeof(tmp), "%llu", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(double val) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%g", val);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(float val) {
    return *this << static_cast<double>(val);
}

LOutputStream& LOutputStream::operator<<(bool val) {
    return *this << (val ? "true" : "false");
}

LOutputStream& LOutputStream::operator<<(const void* ptr) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%p", ptr);
    buf.write(applyFormat(tmp));
    return *this;
}

LOutputStream& LOutputStream::operator<<(LOutputStream& (*manip)(LOutputStream&)) {
    return manip(*this);
}

LOutputStream& LOutputStream::operator<<(LSetW sw) {
    fieldWidth = sw.width;
    return *this;
}

void LOutputStream::write(const char* data, size_t len) {
    buf.write(data, len);
}

void LOutputStream::setWidth(int w) { fieldWidth = w; }
void LOutputStream::setLeftAlign(bool align) { leftAlign = align; }
void LOutputStream::setBase(int base) { numBase = base; }

void LOutputStream::setOutputCallback(LStreamBuf::OutputCallback cb) { 
    buf.setOutputCallback(cb); 
}

void LOutputStream::enableDirectConsole(bool enable) { 
    buf.enableDirectConsole(enable); 
}

void LOutputStream::enableBuffer(bool enable) { 
    buf.enableBuffer(enable); 
}

void LOutputStream::setMaxHistory(size_t max) { 
    buf.setMaxHistory(max); 
}

const deque<string>& LOutputStream::getHistory() const { 
    return buf.getHistory(); 
}

void LOutputStream::clearHistory() { 
    buf.clearHistory(); 
}

string LOutputStream::getHistoryAsString() const { 
    return buf.getHistoryAsString(); 
}

size_t LOutputStream::getHistorySize() const { 
    return buf.getHistorySize(); 
}

void LOutputStream::flush() {
    buf.flush();
}

LStreamBuf* LOutputStream::rdbuf() { 
    return &buf; 
}

LInputStream::LInputStream() {}

LInputStream& LInputStream::operator>>(string& s) {
    s = buf.readLine();
    return *this;
}

LInputStream& LInputStream::operator>>(int& val) {
    string s = buf.readLine();
    val = atoi(s.c_str());
    return *this;
}

LInputStream& LInputStream::operator>>(double& val) {
    string s = buf.readLine();
    val = atof(s.c_str());
    return *this;
}

LInputStream& LInputStream::operator>>(char& c) {
    int ch = buf.read();
    c = (ch >= 0) ? static_cast<char>(ch) : '\0';
    return *this;
}

bool LInputStream::getline(string& line) {
    line = buf.readLine();
    return !line.empty() || buf.hasData();
}

void LInputStream::setInputCallback(function<string()> cb) { 
    buf.setInputCallback(cb); 
}

void LInputStream::feedInput(const string& text) { 
    buf.feedInput(text); 
}

void LInputStream::clearInput() { 
    buf.clearBuffer(); 
}

bool LInputStream::hasData() const {
    return buf.hasData();
}

LStream::LStream(size_t historySize) : out(historySize) {}

LOutputStream& LStream::output() { 
    return out; 
}

LInputStream& LStream::input() { 
    return in; 
}

void LStream::setOutputCallback(LStreamBuf::OutputCallback cb) { 
    out.setOutputCallback(cb); 
}

void LStream::setInputCallback(function<string()> cb) { 
    in.setInputCallback(cb); 
}

void LStream::enableDirectConsole(bool enable) {
    out.enableDirectConsole(enable);
}

const deque<string>& LStream::getHistory() const { 
    return out.getHistory(); 
}

void LStream::clearHistory() { 
    out.clearHistory(); 
}

void LStream::feedInput(const string& text) { 
    in.feedInput(text); 
}

LOutputStream& endl(LOutputStream& os) {
    os << '\n';
    os.flush();
    return os;
}

LOutputStream& flush(LOutputStream& os) {
    os.flush();
    return os;
}

LStream lstream(5000);
