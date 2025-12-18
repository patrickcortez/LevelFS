/*
 * vfs.hpp - Virtual File System Layer for LevelFS
 * 
 * Compile: Include in mount.cpp with #include "vfs.hpp"
 */

#ifndef LFS_VFS_HPP
#define LFS_VFS_HPP

#include "fs_common.hpp"
#include "error.hpp"
#include "concurrent.hpp"
#include "journal.hpp"
#include "permissions.hpp"
#include "fs_entry.hpp"
#include "fs_context.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

using namespace std;

typedef int64_t VFSHandle;

#define VFS_HANDLE_INVALID -1

enum VFSOpenMode {
    VFS_MODE_READ   = 1 << 0,
    VFS_MODE_WRITE  = 1 << 1,
    VFS_MODE_APPEND = 1 << 2,
    VFS_MODE_CREATE = 1 << 3,
    VFS_MODE_TRUNC  = 1 << 4
};

enum VFSSeekMode {
    VFS_SEEK_SET = 0,
    VFS_SEEK_CUR = 1,
    VFS_SEEK_END = 2
};

enum VFSEntryType {
    VFS_TYPE_UNKNOWN = 0,
    VFS_TYPE_FILE    = 1,
    VFS_TYPE_DIR     = 2,
    VFS_TYPE_SYMLINK = 3,
    VFS_TYPE_LEVEL   = 4
};

struct VFSStat {
    VFSEntryType type;
    uint64_t size;
    uint32_t permissions;
    uint32_t createTime;
    uint32_t modTime;
    uint64_t startCluster;
    uint32_t levelID;
    char name[25];
    char extension[9];
    
    VFSStat() : type(VFS_TYPE_UNKNOWN), size(0), permissions(0),
                createTime(0), modTime(0), startCluster(0), levelID(0) {
        memset(name, 0, sizeof(name));
        memset(extension, 0, sizeof(extension));
    }
};

struct VFSDirEntry {
    string name;
    VFSEntryType type;
    uint64_t size;
    uint32_t permissions;
};

struct VFSOpenFile {
    VFSHandle handle;
    uint64_t startCluster;
    uint64_t size;
    uint64_t position;
    int mode;
    string path;
    bool valid;
    
    VFSOpenFile() : handle(VFS_HANDLE_INVALID), startCluster(0), size(0),
                    position(0), mode(0), valid(false) {}
};

class LevelVFS {
private:
    DiskDevice disk;
    SuperBlock sb;
    Journal* journal;
    FileLockManager lockManager;
    PermissionCache permCache;
    EntryReader* entryReader;
    EntryWriter* entryWriter;
    EntryFinder* entryFinder;
    
    bool mounted;
    string mountedPath;
    
    NavigationContext context;
    
    unordered_map<VFSHandle, VFSOpenFile> openFiles;
    VFSHandle nextHandle;
    
    uint64_t allocCluster();
    uint64_t allocClusterForLevel(uint32_t levelID);
    void freeCluster(uint64_t cluster);
    void freeChain(uint64_t startCluster);
    
    vector<uint64_t> getChain(uint64_t startCluster);
    LABEntry getLABEntry(uint64_t cluster);
    void setLABEntry(uint64_t cluster, LABEntry value);
    
    bool isReservedCluster(uint64_t cluster);
    
    struct PathResult {
        uint64_t parentCluster;
        string name;
        bool valid;
    };
    
    PathResult resolvePath(const string& path);
    FindResult findEntry(uint64_t contentCluster, const string& name);
    
    int readClusterData(uint64_t cluster, void* buffer);
    int writeClusterData(uint64_t cluster, const void* buffer);
    
    void writeSuperBlock();
    bool tryBackupSuperblock();

public:
    LevelVFS();
    ~LevelVFS();
    
    int mount(char driveLetter);
    int mountAuto();
    int mountImage(const string& path);
    int mountDirect(int diskIndex, uint64_t offset);
    void unmount();
    bool isMounted() const { return mounted; }
    
    const SuperBlock& getSuperBlock() const { return sb; }
    const NavigationContext& getContext() const { return context; }
    
    int loadLevel(const string& levelName);
    int loadLevelByID(uint32_t levelID);
    vector<string> listLevels();
    
    LfsResultValue<VFSHandle> open(const string& path, int mode);
    int read(VFSHandle handle, void* buffer, size_t size);
    int write(VFSHandle handle, const void* buffer, size_t size);
    int seek(VFSHandle handle, int64_t offset, VFSSeekMode whence);
    int64_t tell(VFSHandle handle);
    int close(VFSHandle handle);
    
    int mkdir(const string& path);
    int touch(const string& path, const string& extension = "");
    int remove(const string& path);
    int rmdir(const string& path);
    int rename(const string& oldPath, const string& newPath);
    
    LfsResultValue<vector<VFSDirEntry>> listDir(const string& path);
    LfsResultValue<VFSStat> stat(const string& path);
    bool exists(const string& path);
    
    int createLevel(const string& parentPath, const string& levelName);
    int deleteLevel(const string& levelPath);
    
    int chmod(const string& path, uint32_t permissions);
    
    uint64_t getFreeSpace();
    uint64_t getTotalSpace();
    uint64_t getUsedSpace();
    
    int sync();
};

#endif
