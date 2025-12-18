/*
 * error.hpp - Error Handling for LevelFS VFS
 * 
 * Compile: Include in vfs.hpp with #include "error.hpp"
 */

#ifndef LFS_ERROR_HPP
#define LFS_ERROR_HPP

#include <string>
#include <exception>
#include <cstdint>

using namespace std;

#define LFS_SUCCESS          0
#define LFS_ERR_GENERAL     -1
#define LFS_ERR_NOT_MOUNTED -2
#define LFS_ERR_NOT_FOUND   -3
#define LFS_ERR_PERMISSION  -4
#define LFS_ERR_DISK_IO     -5
#define LFS_ERR_INVALID     -6
#define LFS_ERR_EXISTS      -7
#define LFS_ERR_NO_SPACE    -8
#define LFS_ERR_NOT_EMPTY   -9
#define LFS_ERR_IS_DIR      -10
#define LFS_ERR_NOT_DIR     -11
#define LFS_ERR_CORRUPT     -12
#define LFS_ERR_LOCKED      -13
#define LFS_ERR_BUSY        -14
#define LFS_ERR_EOF         -15

inline const char* lfs_strerror(int code) {
    switch (code) {
        case LFS_SUCCESS:          return "Success";
        case LFS_ERR_GENERAL:      return "General error";
        case LFS_ERR_NOT_MOUNTED:  return "Filesystem not mounted";
        case LFS_ERR_NOT_FOUND:    return "File or directory not found";
        case LFS_ERR_PERMISSION:   return "Permission denied";
        case LFS_ERR_DISK_IO:      return "Disk I/O error";
        case LFS_ERR_INVALID:      return "Invalid argument";
        case LFS_ERR_EXISTS:       return "File or directory already exists";
        case LFS_ERR_NO_SPACE:     return "No space left on device";
        case LFS_ERR_NOT_EMPTY:    return "Directory not empty";
        case LFS_ERR_IS_DIR:       return "Is a directory";
        case LFS_ERR_NOT_DIR:      return "Not a directory";
        case LFS_ERR_CORRUPT:      return "Filesystem corrupted";
        case LFS_ERR_LOCKED:       return "Resource locked";
        case LFS_ERR_BUSY:         return "Resource busy";
        case LFS_ERR_EOF:          return "End of file";
        default:                   return "Unknown error";
    }
}

#define LFS_FAIL(code) return (code)

#define LFS_ENSURE(cond, code) \
    do { if (!(cond)) return (code); } while(0)

#define LFS_ENSURE_MOUNTED() \
    do { if (!mounted) return LFS_ERR_NOT_MOUNTED; } while(0)

#define LFS_TRY(expr) \
    do { \
        int _lfs_rc = (expr); \
        if (_lfs_rc != LFS_SUCCESS) return _lfs_rc; \
    } while(0)

#define LFS_TRY_OR(expr, cleanup) \
    do { \
        int _lfs_rc = (expr); \
        if (_lfs_rc != LFS_SUCCESS) { cleanup; return _lfs_rc; } \
    } while(0)

#define LFS_ASSERT(cond, msg) \
    do { \
        if (!(cond)) throw LfsException(LFS_ERR_GENERAL, msg); \
    } while(0)

class LfsException : public exception {
private:
    int errorCode;
    string message;
    string fullMessage;

public:
    LfsException(int code, const string& msg = "") 
        : errorCode(code), message(msg) {
        fullMessage = string(lfs_strerror(code));
        if (!message.empty()) {
            fullMessage += ": " + message;
        }
    }
    
    const char* what() const noexcept override {
        return fullMessage.c_str();
    }
    
    int code() const noexcept { return errorCode; }
    const string& msg() const noexcept { return message; }
};

class LfsNotMountedException : public LfsException {
public:
    LfsNotMountedException() : LfsException(LFS_ERR_NOT_MOUNTED) {}
};

class LfsNotFoundException : public LfsException {
public:
    LfsNotFoundException(const string& path = "") 
        : LfsException(LFS_ERR_NOT_FOUND, path) {}
};

class LfsPermissionException : public LfsException {
public:
    LfsPermissionException(const string& path = "") 
        : LfsException(LFS_ERR_PERMISSION, path) {}
};

class LfsDiskException : public LfsException {
public:
    LfsDiskException(const string& detail = "") 
        : LfsException(LFS_ERR_DISK_IO, detail) {}
};

class LfsCorruptException : public LfsException {
public:
    LfsCorruptException(const string& detail = "") 
        : LfsException(LFS_ERR_CORRUPT, detail) {}
};

class LfsNoSpaceException : public LfsException {
public:
    LfsNoSpaceException() : LfsException(LFS_ERR_NO_SPACE) {}
};

class LfsExistsException : public LfsException {
public:
    LfsExistsException(const string& path = "") 
        : LfsException(LFS_ERR_EXISTS, path) {}
};

class LfsLockedException : public LfsException {
public:
    LfsLockedException(const string& path = "") 
        : LfsException(LFS_ERR_LOCKED, path) {}
};

struct LfsResult {
    int code;
    string message;
    
    LfsResult() : code(LFS_SUCCESS) {}
    LfsResult(int c) : code(c), message(lfs_strerror(c)) {}
    LfsResult(int c, const string& m) : code(c), message(m) {}
    
    bool ok() const { return code == LFS_SUCCESS; }
    bool failed() const { return code != LFS_SUCCESS; }
    operator bool() const { return ok(); }
    
    static LfsResult success() { return LfsResult(LFS_SUCCESS); }
    static LfsResult error(int code) { return LfsResult(code); }
    static LfsResult error(int code, const string& msg) { return LfsResult(code, msg); }
};

template<typename T>
struct LfsResultValue {
    int code;
    T value;
    string message;
    
    LfsResultValue() : code(LFS_ERR_GENERAL) {}
    LfsResultValue(T v) : code(LFS_SUCCESS), value(v) {}
    LfsResultValue(int c) : code(c), message(lfs_strerror(c)) {}
    LfsResultValue(int c, const string& m) : code(c), message(m) {}
    
    bool ok() const { return code == LFS_SUCCESS; }
    bool failed() const { return code != LFS_SUCCESS; }
    operator bool() const { return ok(); }
    
    T& get() { return value; }
    const T& get() const { return value; }
};

#endif
