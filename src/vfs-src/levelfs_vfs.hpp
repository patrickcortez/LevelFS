/*
 * levelfs_vfs.hpp - LevelFS VFS Provider Header
 *
 * Compile: Include with #include "levelfs_vfs.hpp"
 *
 * Implements IVfsOperations for LevelFS filesystem.
 */

#ifndef LEVELFS_VFS_HPP
#define LEVELFS_VFS_HPP

#include "vfs_interface.hpp"
#include "vfs_types.hpp"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <functional>

using namespace std;

class FileSystemShell;

class HandleTable {
private:
    unordered_map<uint64_t, LfsHandle> handles;
    atomic<uint64_t> nextId;
    mutex mtx;
    size_t maxHandles;

public:
    HandleTable(size_t max = 1024) : nextId(1), maxHandles(max) {}

    uint64_t allocate(uint64_t cluster, uint32_t flags, uint64_t levelID,
                      uint64_t parentCluster, const char* name, const char* level) {
        lock_guard<mutex> lock(mtx);
        if (handles.size() >= maxHandles) return LFS_HANDLE_INVALID;

        uint64_t id = nextId++;
        LfsHandle fh;
        fh.id = id;
        fh.cluster = cluster;
        fh.position = 0;
        fh.size = 0;
        fh.flags = flags;
        fh.levelID = levelID;
        fh.parentCluster = parentCluster;
        if (name) strncpy(fh.name, name, LFS_MAX_NAME - 1);
        if (level) strncpy(fh.level, level, LFS_MAX_LEVEL_NAME - 1);
        handles[id] = fh;
        return id;
    }

    LfsHandle* get(uint64_t id) {
        lock_guard<mutex> lock(mtx);
        auto it = handles.find(id);
        if (it == handles.end()) return nullptr;
        return &it->second;
    }

    bool release(uint64_t id) {
        lock_guard<mutex> lock(mtx);
        return handles.erase(id) > 0;
    }

    void updatePosition(uint64_t id, uint64_t pos) {
        lock_guard<mutex> lock(mtx);
        auto it = handles.find(id);
        if (it != handles.end()) it->second.position = pos;
    }

    void updateSize(uint64_t id, uint64_t size) {
        lock_guard<mutex> lock(mtx);
        auto it = handles.find(id);
        if (it != handles.end()) it->second.size = size;
    }

    size_t count() const { return handles.size(); }
    size_t capacity() const { return maxHandles; }

    void forEach(function<void(const LfsHandle&)> fn) {
        lock_guard<mutex> lock(mtx);
        for (const auto& pair : handles) fn(pair.second);
    }
};

struct VfsContext {
    uint64_t rootContentCluster;
    uint64_t currentLevelID;
    string currentLevel;
    string currentPath;
    uint32_t currentPerms;

    VfsContext() : rootContentCluster(0), currentLevelID(0), 
                   currentLevel("master"), currentPath("/"), currentPerms(0) {}
};

class LevelFSProvider : public IVfsOperations {
private:
    FileSystemShell* shell;
    HandleTable handles;
    VfsContext context;
    mutex opMutex;
    bool mounted;

    struct PathInfo {
        uint64_t parentCluster;
        uint64_t targetCluster;
        string name;
        string level;
        uint8_t type;
        bool found;
        uint64_t size;
        uint32_t attrs;
        uint32_t mtime;
    };

    PathInfo resolvePath(const char* path);
    LfsError fillStatFromEntry(const PathInfo& info, LfsStat* st);
    LfsError checkPermission(uint32_t attrs, uint32_t required);

public:
    LevelFSProvider();
    ~LevelFSProvider();

    bool attach(FileSystemShell* fs);
    void detach();
    bool isMounted() const { return mounted; }

    LfsError getattr(const char* path, LfsStat* st) override;
    LfsError readdir(const char* path, LfsFillDirCallback filler) override;
    LfsError open(const char* path, uint32_t flags, LfsHandle* fh) override;
    LfsError create(const char* path, uint32_t mode, LfsHandle* fh) override;
    LfsError release(LfsHandle* fh) override;
    LfsError read(LfsHandle* fh, void* buf, size_t size,
                  uint64_t offset, size_t* bytesRead) override;
    LfsError write(LfsHandle* fh, const void* buf, size_t size,
                   uint64_t offset, size_t* bytesWritten) override;
    LfsError seek(LfsHandle* fh, int64_t offset, int whence, uint64_t* newPos) override;
    LfsError fstat(LfsHandle* fh, LfsStat* st) override;
    LfsError truncate(const char* path, uint64_t size) override;
    LfsError ftruncate(LfsHandle* fh, uint64_t size) override;
    LfsError unlink(const char* path) override;
    LfsError mkdir(const char* path, uint32_t mode) override;
    LfsError rmdir(const char* path) override;
    LfsError rename(const char* from, const char* to) override;
    LfsError symlink(const char* target, const char* linkpath) override;
    LfsError readlink(const char* path, char* buf, size_t size) override;
    LfsError chmod(const char* path, uint32_t mode) override;
    LfsError utimens(const char* path, uint64_t atime, uint64_t mtime) override;
    LfsError statfs(const char* path, LfsStatFs* stfs) override;
    LfsError flush(LfsHandle* fh) override;
    LfsError fsync(LfsHandle* fh, int datasync) override;

    LfsError listLevels(const char* path, LfsFillLevelCallback filler) override;
    LfsError switchLevel(const char* path, const char* level) override;
    LfsError createLevel(const char* path, const char* levelName) override;
    LfsError deleteLevel(const char* path, const char* levelName) override;
    LfsError branchLevel(const char* path, const char* parentLevel,
                         const char* newLevel) override;
    LfsError getCurrentLevel(const char* path, char* levelName, size_t size) override;
};

void vfsSetGlobals(IDiskReader* disk, VfsSuperBlock* sb, VfsNavigationContext* ctx);

#endif
