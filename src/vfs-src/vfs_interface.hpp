/*
 * vfs_interface.hpp - Virtual File System Interface for LevelFS
 *
 * Compile: Include with #include "vfs_interface.hpp"
 *
 * POSIX-like abstraction layer for file operations with error codes.
 * Designed for WinFsp integration on Windows.
 */

#ifndef VFS_INTERFACE_HPP
#define VFS_INTERFACE_HPP

#include <cstdint>
#include <cstring>
#include <functional>

#pragma pack(push, 1)

enum LfsError {
    LFS_OK          = 0,
    LFS_EPERM       = -1,
    LFS_ENOENT      = -2,
    LFS_EIO         = -5,
    LFS_ENXIO       = -6,
    LFS_EBADF       = -9,
    LFS_ENOMEM      = -12,
    LFS_EACCES      = -13,
    LFS_EBUSY       = -16,
    LFS_EEXIST      = -17,
    LFS_EXDEV       = -18,
    LFS_ENOTDIR     = -20,
    LFS_EISDIR      = -21,
    LFS_EINVAL      = -22,
    LFS_ENFILE      = -23,
    LFS_EMFILE      = -24,
    LFS_EFBIG       = -27,
    LFS_ENOSPC      = -28,
    LFS_EROFS       = -30,
    LFS_ENAMETOOLONG = -36,
    LFS_ENOTEMPTY   = -39,
    LFS_ELOOP       = -40,
    LFS_ENODATA     = -61,
    LFS_ENOTMOUNTED = -100,
    LFS_ELEVEL      = -101,
    LFS_ELOCKED     = -102
};

inline const char* lfsErrorString(LfsError err) {
    switch (err) {
        case LFS_OK:          return "Success";
        case LFS_EPERM:       return "Operation not permitted";
        case LFS_ENOENT:      return "No such file or directory";
        case LFS_EIO:         return "I/O error";
        case LFS_ENXIO:       return "No such device or address";
        case LFS_EBADF:       return "Bad file descriptor";
        case LFS_ENOMEM:      return "Out of memory";
        case LFS_EACCES:      return "Permission denied";
        case LFS_EBUSY:       return "Resource busy";
        case LFS_EEXIST:      return "File exists";
        case LFS_EXDEV:       return "Cross-device link";
        case LFS_ENOTDIR:     return "Not a directory";
        case LFS_EISDIR:      return "Is a directory";
        case LFS_EINVAL:      return "Invalid argument";
        case LFS_ENFILE:      return "Too many open files in system";
        case LFS_EMFILE:      return "Too many open files";
        case LFS_EFBIG:       return "File too large";
        case LFS_ENOSPC:      return "No space left on device";
        case LFS_EROFS:       return "Read-only file system";
        case LFS_ENAMETOOLONG:return "Filename too long";
        case LFS_ENOTEMPTY:   return "Directory not empty";
        case LFS_ELOOP:       return "Too many symbolic links";
        case LFS_ENODATA:     return "No data available";
        case LFS_ENOTMOUNTED: return "Filesystem not mounted";
        case LFS_ELEVEL:      return "Level not found";
        case LFS_ELOCKED:     return "File is locked";
        default:              return "Unknown error";
    }
}

#define LFS_O_RDONLY   0x0001
#define LFS_O_WRONLY   0x0002
#define LFS_O_RDWR     0x0003
#define LFS_O_APPEND   0x0008
#define LFS_O_CREAT    0x0100
#define LFS_O_TRUNC    0x0200
#define LFS_O_EXCL     0x0400

#define LFS_SEEK_SET   0
#define LFS_SEEK_CUR   1
#define LFS_SEEK_END   2

#define LFS_S_IFMT     0170000
#define LFS_S_IFREG    0100000
#define LFS_S_IFDIR    0040000
#define LFS_S_IFLNK    0120000

#define LFS_S_IRUSR    0000400
#define LFS_S_IWUSR    0000200
#define LFS_S_IXUSR    0000100
#define LFS_S_IRGRP    0000040
#define LFS_S_IWGRP    0000020
#define LFS_S_IXGRP    0000010
#define LFS_S_IROTH    0000004
#define LFS_S_IWOTH    0000002
#define LFS_S_IXOTH    0000001

#define LFS_S_ISREG(m) (((m) & LFS_S_IFMT) == LFS_S_IFREG)
#define LFS_S_ISDIR(m) (((m) & LFS_S_IFMT) == LFS_S_IFDIR)
#define LFS_S_ISLNK(m) (((m) & LFS_S_IFMT) == LFS_S_IFLNK)

#define LFS_HANDLE_INVALID 0
#define LFS_MAX_PATH       260
#define LFS_MAX_NAME       24
#define LFS_MAX_LEVEL_NAME 32

struct LfsHandle {
    uint64_t id;
    uint64_t cluster;
    uint64_t position;
    uint64_t size;
    uint32_t flags;
    uint64_t levelID;
    uint64_t parentCluster;
    char name[LFS_MAX_NAME];
    char level[LFS_MAX_LEVEL_NAME];

    LfsHandle() : id(LFS_HANDLE_INVALID), cluster(0), position(0), size(0),
                  flags(0), levelID(0), parentCluster(0) {
        memset(name, 0, sizeof(name));
        memset(level, 0, sizeof(level));
    }

    bool isValid() const { return id != LFS_HANDLE_INVALID; }
    bool isReadable() const { return (flags & LFS_O_RDONLY) || (flags & LFS_O_RDWR); }
    bool isWritable() const { return (flags & LFS_O_WRONLY) || (flags & LFS_O_RDWR); }
};

struct LfsStat {
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t levelID;
    char levelName[LFS_MAX_LEVEL_NAME];

    LfsStat() : ino(0), mode(0), nlink(1), uid(0), gid(0), size(0),
                blksize(4096), blocks(0), atime(0), mtime(0), ctime(0), levelID(0) {
        memset(levelName, 0, sizeof(levelName));
    }
};

struct LfsStatFs {
    uint64_t totalBlocks;
    uint64_t freeBlocks;
    uint64_t availBlocks;
    uint64_t totalInodes;
    uint64_t freeInodes;
    uint64_t blockSize;
    uint64_t maxNameLen;
    char volumeName[32];

    LfsStatFs() : totalBlocks(0), freeBlocks(0), availBlocks(0),
                  totalInodes(0), freeInodes(0), blockSize(4096), maxNameLen(24) {
        memset(volumeName, 0, sizeof(volumeName));
    }
};

struct LfsDirEntry {
    char name[LFS_MAX_NAME];
    char extension[4];
    uint8_t type;
    uint64_t size;
    uint64_t cluster;
    uint32_t mtime;

    LfsDirEntry() : type(0), size(0), cluster(0), mtime(0) {
        memset(name, 0, sizeof(name));
        memset(extension, 0, sizeof(extension));
    }
};

struct LfsLevelInfo {
    char name[LFS_MAX_LEVEL_NAME];
    uint64_t levelID;
    uint64_t parentLevelID;
    uint64_t contentCluster;
    uint32_t flags;
    uint8_t isActive;
    uint8_t isLocked;
    uint8_t isSnapshot;

    LfsLevelInfo() : levelID(0), parentLevelID(0), contentCluster(0),
                     flags(0), isActive(0), isLocked(0), isSnapshot(0) {
        memset(name, 0, sizeof(name));
    }
};

#pragma pack(pop)

using LfsFillDirCallback = std::function<int(const char* name, const LfsStat* st)>;
using LfsFillLevelCallback = std::function<int(const char* levelName, const LfsLevelInfo* info)>;

class IVfsOperations {
public:
    virtual ~IVfsOperations() = default;

    virtual LfsError getattr(const char* path, LfsStat* st) = 0;

    virtual LfsError readdir(const char* path, LfsFillDirCallback filler) = 0;

    virtual LfsError open(const char* path, uint32_t flags, LfsHandle* fh) = 0;
    virtual LfsError create(const char* path, uint32_t mode, LfsHandle* fh) = 0;
    virtual LfsError release(LfsHandle* fh) = 0;

    virtual LfsError read(LfsHandle* fh, void* buf, size_t size, 
                          uint64_t offset, size_t* bytesRead) = 0;
    virtual LfsError write(LfsHandle* fh, const void* buf, size_t size, 
                           uint64_t offset, size_t* bytesWritten) = 0;

    virtual LfsError seek(LfsHandle* fh, int64_t offset, int whence, uint64_t* newPos) = 0;
    virtual LfsError fstat(LfsHandle* fh, LfsStat* st) = 0;

    virtual LfsError truncate(const char* path, uint64_t size) = 0;
    virtual LfsError ftruncate(LfsHandle* fh, uint64_t size) = 0;

    virtual LfsError unlink(const char* path) = 0;

    virtual LfsError mkdir(const char* path, uint32_t mode) = 0;
    virtual LfsError rmdir(const char* path) = 0;

    virtual LfsError rename(const char* from, const char* to) = 0;

    virtual LfsError symlink(const char* target, const char* linkpath) = 0;
    virtual LfsError readlink(const char* path, char* buf, size_t size) = 0;

    virtual LfsError chmod(const char* path, uint32_t mode) = 0;
    virtual LfsError utimens(const char* path, uint64_t atime, uint64_t mtime) = 0;

    virtual LfsError statfs(const char* path, LfsStatFs* stfs) = 0;

    virtual LfsError flush(LfsHandle* fh) = 0;
    virtual LfsError fsync(LfsHandle* fh, int datasync) = 0;

    virtual LfsError listLevels(const char* path, LfsFillLevelCallback filler) = 0;
    virtual LfsError switchLevel(const char* path, const char* level) = 0;
    virtual LfsError createLevel(const char* path, const char* levelName) = 0;
    virtual LfsError deleteLevel(const char* path, const char* levelName) = 0;
    virtual LfsError branchLevel(const char* path, const char* parentLevel, 
                                 const char* newLevel) = 0;
    virtual LfsError getCurrentLevel(const char* path, char* levelName, size_t size) = 0;
};

class VfsOperationsBase : public IVfsOperations {
public:
    LfsError getattr(const char*, LfsStat*) override { return LFS_ENOENT; }
    LfsError readdir(const char*, LfsFillDirCallback) override { return LFS_ENOTDIR; }
    LfsError open(const char*, uint32_t, LfsHandle*) override { return LFS_ENOENT; }
    LfsError create(const char*, uint32_t, LfsHandle*) override { return LFS_EACCES; }
    LfsError release(LfsHandle*) override { return LFS_OK; }
    LfsError read(LfsHandle*, void*, size_t, uint64_t, size_t*) override { return LFS_EBADF; }
    LfsError write(LfsHandle*, const void*, size_t, uint64_t, size_t*) override { return LFS_EBADF; }
    LfsError seek(LfsHandle*, int64_t, int, uint64_t*) override { return LFS_EBADF; }
    LfsError fstat(LfsHandle*, LfsStat*) override { return LFS_EBADF; }
    LfsError truncate(const char*, uint64_t) override { return LFS_EACCES; }
    LfsError ftruncate(LfsHandle*, uint64_t) override { return LFS_EBADF; }
    LfsError unlink(const char*) override { return LFS_EACCES; }
    LfsError mkdir(const char*, uint32_t) override { return LFS_EACCES; }
    LfsError rmdir(const char*) override { return LFS_EACCES; }
    LfsError rename(const char*, const char*) override { return LFS_EACCES; }
    LfsError symlink(const char*, const char*) override { return LFS_EACCES; }
    LfsError readlink(const char*, char*, size_t) override { return LFS_EINVAL; }
    LfsError chmod(const char*, uint32_t) override { return LFS_EACCES; }
    LfsError utimens(const char*, uint64_t, uint64_t) override { return LFS_EACCES; }
    LfsError statfs(const char*, LfsStatFs*) override { return LFS_EIO; }
    LfsError flush(LfsHandle*) override { return LFS_OK; }
    LfsError fsync(LfsHandle*, int) override { return LFS_OK; }
    LfsError listLevels(const char*, LfsFillLevelCallback) override { return LFS_ENOTDIR; }
    LfsError switchLevel(const char*, const char*) override { return LFS_ELEVEL; }
    LfsError createLevel(const char*, const char*) override { return LFS_EACCES; }
    LfsError deleteLevel(const char*, const char*) override { return LFS_EACCES; }
    LfsError branchLevel(const char*, const char*, const char*) override { return LFS_EACCES; }
    LfsError getCurrentLevel(const char*, char*, size_t) override { return LFS_ENOTDIR; }
};

#endif
