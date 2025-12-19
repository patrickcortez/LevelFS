/*
 * levelfs_vfs.cpp - LevelFS VFS Provider Implementation
 *
 * Compile: g++ -c levelfs_vfs.cpp -o levelfs_vfs.o -std=c++17
 *
 * Implements IVfsOperations interface for LevelFS.
 */

#include "levelfs_vfs.hpp"
#include "vfs_types.hpp"
#include <ctime>
#include <algorithm>
#include <vector>
#include <sstream>

using namespace std;

static IDiskReader* vfsDiskPtr = nullptr;
static VfsSuperBlock* vfsSbPtr = nullptr;
static VfsNavigationContext* vfsContextPtr = nullptr;

static IDiskReader* getDisk() { return vfsDiskPtr; }
static VfsSuperBlock* getSb() { return vfsSbPtr; }
static VfsNavigationContext* getContext() { return vfsContextPtr; }

static vector<string> splitPath(const string& path) {
    vector<string> parts;
    stringstream ss(path);
    string part;
    while (getline(ss, part, '/')) {
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(part);
            }
        }
    }
    return parts;
}

LevelFSProvider::LevelFSProvider() : shell(nullptr), handles(1024), mounted(false) {}

LevelFSProvider::~LevelFSProvider() {
    detach();
}

bool LevelFSProvider::attach(FileSystemShell* fs) {
    lock_guard<mutex> lock(opMutex);
    if (mounted) return false;
    shell = fs;
    mounted = true;
    return true;
}

void LevelFSProvider::detach() {
    lock_guard<mutex> lock(opMutex);
    shell = nullptr;
    mounted = false;
}

LevelFSProvider::PathInfo LevelFSProvider::resolvePath(const char* path) {
    PathInfo info;
    info.parentCluster = 0;
    info.targetCluster = 0;
    info.found = false;
    info.type = 0;
    info.size = 0;
    info.attrs = 0;
    info.mtime = 0;

    if (!path || !mounted) return info;

    IDiskReader* disk = getDisk();
    VfsSuperBlock* sb = getSb();
    VfsNavigationContext* ctx = getContext();
    if (!disk || !sb || !ctx || !disk->isOpen()) return info;

    string pathStr(path);
    if (pathStr.empty() || pathStr == "/") {
        info.parentCluster = ctx->rootContentCluster;
        info.targetCluster = ctx->rootContentCluster;
        info.found = true;
        info.type = TYPE_LEVELED_DIR;
        info.level = "master";
        return info;
    }

    if (pathStr[0] == '/') pathStr = pathStr.substr(1);
    if (pathStr.empty()) {
        info.parentCluster = ctx->rootContentCluster;
        info.targetCluster = ctx->rootContentCluster;
        info.found = true;
        info.type = TYPE_LEVELED_DIR;
        info.level = "master";
        return info;
    }

    vector<string> parts = splitPath(pathStr);
    if (parts.empty()) {
        info.parentCluster = ctx->rootContentCluster;
        info.targetCluster = ctx->rootContentCluster;
        info.found = true;
        info.type = TYPE_LEVELED_DIR;
        info.level = "master";
        return info;
    }

    uint64_t current = ctx->rootContentCluster;
    string currentLevel = "master";

    for (size_t i = 0; i < parts.size(); i++) {
        string part = parts[i];
        string levelName = "master";
        size_t colon = part.find(':');
        if (colon != string::npos) {
            levelName = part.substr(colon + 1);
            part = part.substr(0, colon);
        }

        VfsDirEntry entries[SECTOR_SIZE / sizeof(VfsDirEntry)];
        bool found = false;
        uint64_t foundCluster = 0;
        uint8_t foundType = 0;
        uint64_t foundSize = 0;
        uint32_t foundAttrs = 0;
        uint32_t foundMtime = 0;

        uint64_t searchCluster = current;
        while (searchCluster != 0 && searchCluster != LAT_END && searchCluster < sb->totalClusters) {
            for (int s = 0; s < 8 && !found; s++) {
                memset(entries, 0, sizeof(entries));
                disk->readSector(searchCluster * 8 + s, entries);
                for (int j = 0; j < SECTOR_SIZE / (int)sizeof(VfsDirEntry); j++) {
                    if (entries[j].type != TYPE_FREE && string(entries[j].name) == part) {
                        foundCluster = entries[j].startCluster;
                        foundType = entries[j].type;
                        foundSize = entries[j].size;
                        foundAttrs = entries[j].attributes;
                        foundMtime = entries[j].modTime;
                        found = true;
                        break;
                    }
                }
            }
            if (found) break;
            
            char labBuf[CLUSTER_SIZE];
            uint64_t litIndex = searchCluster / CLUSTERS_PER_LIT_ENTRY;
            uint64_t labOffset = searchCluster % CLUSTERS_PER_LIT_ENTRY;
            uint64_t litClusterIdx = litIndex / (CLUSTER_SIZE / sizeof(VfsLITEntry));
            uint64_t litEntryIdx = litIndex % (CLUSTER_SIZE / sizeof(VfsLITEntry));
            
            char litBuf[CLUSTER_SIZE];
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk->readSector((sb->litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                    litBuf + s * SECTOR_SIZE);
            }
            VfsLITEntry* litEntries = (VfsLITEntry*)litBuf;
            uint64_t labCluster = litEntries[litEntryIdx].labCluster;
            
            if (labCluster == 0 || labCluster == LIT_EMPTY) break;
            
            for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
                disk->readSector(labCluster * SECTORS_PER_CLUSTER + s, labBuf + s * SECTOR_SIZE);
            }
            VfsLABEntry* labEntries = (VfsLABEntry*)labBuf;
            uint64_t nextCluster = labEntries[labOffset].nextCluster;
            
            if (nextCluster == LAT_END || nextCluster == LAT_FREE || nextCluster == 0) break;
            searchCluster = nextCluster;
        }

        if (!found) return info;

        if (i == parts.size() - 1) {
            info.parentCluster = current;
            info.targetCluster = foundCluster;
            info.name = part;
            info.level = currentLevel;
            info.type = foundType;
            info.size = foundSize;
            info.attrs = foundAttrs;
            info.mtime = foundMtime;
            info.found = true;
            return info;
        }

        if (foundType != TYPE_LEVELED_DIR) return info;

        char vpsBuf[SECTOR_SIZE];
        VfsVersionEntry* vps = (VfsVersionEntry*)vpsBuf;
        bool levelFound = false;
        uint64_t nextContent = 0;

        for (int s = 0; s < 8; s++) {
            disk->readSector(foundCluster * 8 + s, vpsBuf);
            for (int j = 0; j < SECTOR_SIZE / (int)sizeof(VfsVersionEntry); j++) {
                if (vps[j].isActive && string(vps[j].versionName) == levelName) {
                    nextContent = vps[j].contentTableCluster;
                    levelFound = true;
                    break;
                }
            }
            if (levelFound) break;
        }

        if (!levelFound) return info;
        current = nextContent;
        currentLevel = levelName;
    }

    return info;
}

LfsError LevelFSProvider::fillStatFromEntry(const PathInfo& info, LfsStat* st) {
    if (!st) return LFS_EINVAL;
    memset(st, 0, sizeof(LfsStat));

    st->ino = info.targetCluster;
    st->size = info.size;
    st->blksize = CLUSTER_SIZE;
    st->blocks = (info.size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    st->mtime = info.mtime;
    st->atime = info.mtime;
    st->ctime = info.mtime;

    if (info.type == TYPE_LEVELED_DIR) {
        st->mode = LFS_S_IFDIR | LFS_S_IRUSR | LFS_S_IWUSR | LFS_S_IXUSR;
    } else if (info.type == TYPE_SYMLINK) {
        st->mode = LFS_S_IFLNK | LFS_S_IRUSR | LFS_S_IWUSR | LFS_S_IXUSR;
    } else {
        st->mode = LFS_S_IFREG;
        if (info.attrs & PERM_READ) st->mode |= LFS_S_IRUSR | LFS_S_IRGRP | LFS_S_IROTH;
        if (info.attrs & PERM_WRITE) st->mode |= LFS_S_IWUSR;
        if (info.attrs & PERM_EXEC) st->mode |= LFS_S_IXUSR | LFS_S_IXGRP | LFS_S_IXOTH;
    }

    st->nlink = 1;
    strncpy(st->levelName, info.level.c_str(), LFS_MAX_LEVEL_NAME - 1);
    return LFS_OK;
}

LfsError LevelFSProvider::checkPermission(uint32_t attrs, uint32_t required) {
    if ((attrs & required) != required) return LFS_EACCES;
    return LFS_OK;
}

LfsError LevelFSProvider::getattr(const char* path, LfsStat* st) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path || !st) return LFS_EINVAL;

    PathInfo info = resolvePath(path);
    if (!info.found) return LFS_ENOENT;

    return fillStatFromEntry(info, st);
}

LfsError LevelFSProvider::readdir(const char* path, LfsFillDirCallback filler) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path) return LFS_EINVAL;

    IDiskReader* disk = getDisk();
    VfsSuperBlock* sb = getSb();
    if (!disk || !sb) return LFS_EIO;

    PathInfo info = resolvePath(path);
    if (!info.found) return LFS_ENOENT;

    uint64_t contentCluster = info.targetCluster;
    if (info.type == TYPE_LEVELED_DIR && strlen(path) > 1) {
        char vpsBuf[SECTOR_SIZE];
        VfsVersionEntry* vps = (VfsVersionEntry*)vpsBuf;
        bool found = false;
        for (int s = 0; s < 8; s++) {
            disk->readSector(info.targetCluster * 8 + s, vpsBuf);
            for (int j = 0; j < SECTOR_SIZE / (int)sizeof(VfsVersionEntry); j++) {
                if (vps[j].isActive) {
                    contentCluster = vps[j].contentTableCluster;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return LFS_EIO;
    }

    LfsStat dotStat;
    memset(&dotStat, 0, sizeof(dotStat));
    dotStat.mode = LFS_S_IFDIR | LFS_S_IRUSR | LFS_S_IWUSR | LFS_S_IXUSR;
    filler(".", &dotStat);
    filler("..", &dotStat);

    uint64_t searchCluster = contentCluster;
    while (searchCluster != 0 && searchCluster != LAT_END && searchCluster < sb->totalClusters) {
        VfsDirEntry entries[SECTOR_SIZE / sizeof(VfsDirEntry)];
        for (int s = 0; s < 8; s++) {
            memset(entries, 0, sizeof(entries));
            disk->readSector(searchCluster * 8 + s, entries);
            for (int j = 0; j < SECTOR_SIZE / (int)sizeof(VfsDirEntry); j++) {
                if (entries[j].type != TYPE_FREE) {
                    LfsStat entryStat;
                    memset(&entryStat, 0, sizeof(entryStat));
                    entryStat.ino = entries[j].startCluster;
                    entryStat.size = entries[j].size;
                    entryStat.mtime = entries[j].modTime;
                    if (entries[j].type == TYPE_LEVELED_DIR) {
                        entryStat.mode = LFS_S_IFDIR | LFS_S_IRUSR | LFS_S_IWUSR | LFS_S_IXUSR;
                    } else if (entries[j].type == TYPE_SYMLINK) {
                        entryStat.mode = LFS_S_IFLNK | LFS_S_IRUSR | LFS_S_IWUSR | LFS_S_IXUSR;
                    } else {
                        entryStat.mode = LFS_S_IFREG | LFS_S_IRUSR | LFS_S_IWUSR;
                    }
                    string name(entries[j].name);
                    if (entries[j].extension[0] != 0) {
                        name += ".";
                        name += entries[j].extension;
                    }
                    filler(name.c_str(), &entryStat);
                }
            }
        }

        char labBuf[CLUSTER_SIZE];
        uint64_t litIndex = searchCluster / CLUSTERS_PER_LIT_ENTRY;
        uint64_t labOffset = searchCluster % CLUSTERS_PER_LIT_ENTRY;
        uint64_t litClusterIdx = litIndex / (CLUSTER_SIZE / sizeof(VfsLITEntry));
        uint64_t litEntryIdx = litIndex % (CLUSTER_SIZE / sizeof(VfsLITEntry));
        
        char litBuf[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector((sb->litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                litBuf + s * SECTOR_SIZE);
        }
        VfsLITEntry* litEntries = (VfsLITEntry*)litBuf;
        uint64_t labCluster = litEntries[litEntryIdx].labCluster;
        
        if (labCluster == 0 || labCluster == LIT_EMPTY) break;
        
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector(labCluster * SECTORS_PER_CLUSTER + s, labBuf + s * SECTOR_SIZE);
        }
        VfsLABEntry* labEntries = (VfsLABEntry*)labBuf;
        uint64_t nextCluster = labEntries[labOffset].nextCluster;
        
        if (nextCluster == LAT_END || nextCluster == LAT_FREE || nextCluster == 0) break;
        searchCluster = nextCluster;
    }

    return LFS_OK;
}

LfsError LevelFSProvider::open(const char* path, uint32_t flags, LfsHandle* fh) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path || !fh) return LFS_EINVAL;

    PathInfo info = resolvePath(path);
    if (!info.found) return LFS_ENOENT;
    if (info.type == TYPE_LEVELED_DIR) return LFS_EISDIR;

    if ((flags & LFS_O_RDWR) || (flags & LFS_O_RDONLY)) {
        LfsError err = checkPermission(info.attrs, PERM_READ);
        if (err != LFS_OK) return err;
    }
    if ((flags & LFS_O_RDWR) || (flags & LFS_O_WRONLY)) {
        LfsError err = checkPermission(info.attrs, PERM_WRITE);
        if (err != LFS_OK) return err;
    }

    uint64_t id = handles.allocate(info.targetCluster, flags, 0, 
                                    info.parentCluster, info.name.c_str(), 
                                    info.level.c_str());
    if (id == LFS_HANDLE_INVALID) return LFS_ENFILE;

    LfsHandle* h = handles.get(id);
    if (h) {
        h->size = info.size;
        *fh = *h;
    }
    return LFS_OK;
}

LfsError LevelFSProvider::create(const char* path, uint32_t mode, LfsHandle* fh) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path || !fh) return LFS_EINVAL;
    return LFS_EACCES;
}

LfsError LevelFSProvider::release(LfsHandle* fh) {
    lock_guard<mutex> lock(opMutex);
    if (!fh || !fh->isValid()) return LFS_EBADF;
    handles.release(fh->id);
    return LFS_OK;
}

LfsError LevelFSProvider::read(LfsHandle* fh, void* buf, size_t size,
                               uint64_t offset, size_t* bytesRead) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!fh || !buf || !bytesRead) return LFS_EINVAL;
    if (!fh->isValid()) return LFS_EBADF;
    if (!fh->isReadable()) return LFS_EACCES;

    IDiskReader* disk = getDisk();
    VfsSuperBlock* sb = getSb();
    if (!disk || !sb) return LFS_EIO;

    *bytesRead = 0;
    if (offset >= fh->size) return LFS_OK;

    size_t toRead = min(size, (size_t)(fh->size - offset));
    uint8_t* outBuf = (uint8_t*)buf;

    uint64_t clusterIndex = offset / CLUSTER_SIZE;
    uint64_t clusterOffset = offset % CLUSTER_SIZE;

    uint64_t currentCluster = fh->cluster;
    for (uint64_t i = 0; i < clusterIndex && currentCluster != LAT_END; i++) {
        char labBuf[CLUSTER_SIZE];
        uint64_t litIndex = currentCluster / CLUSTERS_PER_LIT_ENTRY;
        uint64_t labOff = currentCluster % CLUSTERS_PER_LIT_ENTRY;
        uint64_t litClusterIdx = litIndex / (CLUSTER_SIZE / sizeof(VfsLITEntry));
        uint64_t litEntryIdx = litIndex % (CLUSTER_SIZE / sizeof(VfsLITEntry));
        
        char litBuf[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector((sb->litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                litBuf + s * SECTOR_SIZE);
        }
        VfsLITEntry* litEntries = (VfsLITEntry*)litBuf;
        uint64_t labCluster = litEntries[litEntryIdx].labCluster;
        if (labCluster == 0 || labCluster == LIT_EMPTY) break;
        
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector(labCluster * SECTORS_PER_CLUSTER + s, labBuf + s * SECTOR_SIZE);
        }
        VfsLABEntry* labEntries = (VfsLABEntry*)labBuf;
        currentCluster = labEntries[labOff].nextCluster;
    }

    size_t totalRead = 0;
    while (totalRead < toRead && currentCluster != 0 && 
           currentCluster != LAT_END && currentCluster < sb->totalClusters) {
        char clusterBuf[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector(currentCluster * SECTORS_PER_CLUSTER + s, 
                            clusterBuf + s * SECTOR_SIZE);
        }

        size_t copyStart = (totalRead == 0) ? clusterOffset : 0;
        size_t copyLen = min((size_t)(CLUSTER_SIZE - copyStart), toRead - totalRead);
        memcpy(outBuf + totalRead, clusterBuf + copyStart, copyLen);
        totalRead += copyLen;

        char labBuf[CLUSTER_SIZE];
        uint64_t litIndex = currentCluster / CLUSTERS_PER_LIT_ENTRY;
        uint64_t labOff = currentCluster % CLUSTERS_PER_LIT_ENTRY;
        uint64_t litClusterIdx = litIndex / (CLUSTER_SIZE / sizeof(VfsLITEntry));
        uint64_t litEntryIdx = litIndex % (CLUSTER_SIZE / sizeof(VfsLITEntry));
        
        char litBuf[CLUSTER_SIZE];
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector((sb->litStartCluster + litClusterIdx) * SECTORS_PER_CLUSTER + s,
                litBuf + s * SECTOR_SIZE);
        }
        VfsLITEntry* litEntries = (VfsLITEntry*)litBuf;
        uint64_t labCluster = litEntries[litEntryIdx].labCluster;
        if (labCluster == 0 || labCluster == LIT_EMPTY) break;
        
        for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
            disk->readSector(labCluster * SECTORS_PER_CLUSTER + s, labBuf + s * SECTOR_SIZE);
        }
        VfsLABEntry* labEntries = (VfsLABEntry*)labBuf;
        currentCluster = labEntries[labOff].nextCluster;
    }

    *bytesRead = totalRead;
    return LFS_OK;
}

LfsError LevelFSProvider::write(LfsHandle* fh, const void* buf, size_t size,
                                uint64_t offset, size_t* bytesWritten) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!fh || !buf || !bytesWritten) return LFS_EINVAL;
    if (!fh->isValid()) return LFS_EBADF;
    if (!fh->isWritable()) return LFS_EACCES;
    *bytesWritten = 0;
    return LFS_EACCES;
}

LfsError LevelFSProvider::seek(LfsHandle* fh, int64_t offset, int whence, uint64_t* newPos) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!fh || !newPos) return LFS_EINVAL;
    if (!fh->isValid()) return LFS_EBADF;

    uint64_t pos = fh->position;
    int64_t newOffset = 0;

    switch (whence) {
        case LFS_SEEK_SET:
            newOffset = offset;
            break;
        case LFS_SEEK_CUR:
            newOffset = (int64_t)pos + offset;
            break;
        case LFS_SEEK_END:
            newOffset = (int64_t)fh->size + offset;
            break;
        default:
            return LFS_EINVAL;
    }

    if (newOffset < 0) return LFS_EINVAL;
    
    fh->position = (uint64_t)newOffset;
    handles.updatePosition(fh->id, fh->position);
    *newPos = fh->position;
    return LFS_OK;
}

LfsError LevelFSProvider::fstat(LfsHandle* fh, LfsStat* st) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!fh || !st) return LFS_EINVAL;
    if (!fh->isValid()) return LFS_EBADF;

    memset(st, 0, sizeof(LfsStat));
    st->ino = fh->cluster;
    st->size = fh->size;
    st->blksize = CLUSTER_SIZE;
    st->blocks = (fh->size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;
    st->nlink = 1;
    st->mode = LFS_S_IFREG;
    if (fh->isReadable()) st->mode |= LFS_S_IRUSR | LFS_S_IRGRP | LFS_S_IROTH;
    if (fh->isWritable()) st->mode |= LFS_S_IWUSR;
    strncpy(st->levelName, fh->level, LFS_MAX_LEVEL_NAME - 1);
    return LFS_OK;
}

LfsError LevelFSProvider::truncate(const char* path, uint64_t size) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::ftruncate(LfsHandle* fh, uint64_t size) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!fh || !fh->isValid()) return LFS_EBADF;
    return LFS_EACCES;
}

LfsError LevelFSProvider::unlink(const char* path) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::mkdir(const char* path, uint32_t mode) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::rmdir(const char* path) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::rename(const char* from, const char* to) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::symlink(const char* target, const char* linkpath) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::readlink(const char* path, char* buf, size_t size) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path || !buf) return LFS_EINVAL;

    PathInfo info = resolvePath(path);
    if (!info.found) return LFS_ENOENT;
    if (info.type != TYPE_SYMLINK) return LFS_EINVAL;

    IDiskReader* disk = getDisk();
    if (!disk) return LFS_EIO;

    char targetBuf[CLUSTER_SIZE];
    memset(targetBuf, 0, CLUSTER_SIZE);
    for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
        disk->readSector(info.targetCluster * SECTORS_PER_CLUSTER + s, 
                        targetBuf + s * SECTOR_SIZE);
    }

    strncpy(buf, targetBuf, size - 1);
    buf[size - 1] = 0;
    return LFS_OK;
}

LfsError LevelFSProvider::chmod(const char* path, uint32_t mode) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::utimens(const char* path, uint64_t atime, uint64_t mtime) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::statfs(const char* path, LfsStatFs* stfs) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!stfs) return LFS_EINVAL;

    VfsSuperBlock* sb = getSb();
    if (!sb) return LFS_EIO;

    memset(stfs, 0, sizeof(LfsStatFs));
    stfs->totalBlocks = sb->totalClusters;
    stfs->freeBlocks = sb->totalFreeClusters;
    stfs->availBlocks = sb->totalFreeClusters;
    stfs->blockSize = CLUSTER_SIZE;
    stfs->maxNameLen = LFS_MAX_NAME;
    strncpy(stfs->volumeName, sb->volumeName, sizeof(stfs->volumeName) - 1);
    return LFS_OK;
}

LfsError LevelFSProvider::flush(LfsHandle* fh) {
    return LFS_OK;
}

LfsError LevelFSProvider::fsync(LfsHandle* fh, int datasync) {
    return LFS_OK;
}

LfsError LevelFSProvider::listLevels(const char* path, LfsFillLevelCallback filler) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path) return LFS_EINVAL;

    IDiskReader* disk = getDisk();
    if (!disk) return LFS_EIO;

    PathInfo info = resolvePath(path);
    if (!info.found) return LFS_ENOENT;
    if (info.type != TYPE_LEVELED_DIR) return LFS_ENOTDIR;

    char vpsBuf[SECTOR_SIZE];
    VfsVersionEntry* vps = (VfsVersionEntry*)vpsBuf;
    for (int s = 0; s < 8; s++) {
        disk->readSector(info.targetCluster * 8 + s, vpsBuf);
        for (int j = 0; j < SECTOR_SIZE / (int)sizeof(VfsVersionEntry); j++) {
            if (vps[j].isActive) {
                LfsLevelInfo lvlInfo;
                memset(&lvlInfo, 0, sizeof(lvlInfo));
                strncpy(lvlInfo.name, vps[j].versionName, LFS_MAX_LEVEL_NAME - 1);
                lvlInfo.levelID = vps[j].levelID;
                lvlInfo.parentLevelID = vps[j].parentLevelID;
                lvlInfo.contentCluster = vps[j].contentTableCluster;
                lvlInfo.flags = vps[j].flags;
                lvlInfo.isActive = vps[j].isActive;
                lvlInfo.isLocked = vps[j].isLocked;
                lvlInfo.isSnapshot = vps[j].isSnapshot;
                filler(vps[j].versionName, &lvlInfo);
            }
        }
    }

    return LFS_OK;
}

LfsError LevelFSProvider::switchLevel(const char* path, const char* level) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_ELEVEL;
}

LfsError LevelFSProvider::createLevel(const char* path, const char* levelName) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::deleteLevel(const char* path, const char* levelName) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::branchLevel(const char* path, const char* parentLevel,
                                      const char* newLevel) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    return LFS_EACCES;
}

LfsError LevelFSProvider::getCurrentLevel(const char* path, char* levelName, size_t size) {
    lock_guard<mutex> lock(opMutex);
    if (!mounted) return LFS_ENOTMOUNTED;
    if (!path || !levelName) return LFS_EINVAL;

    PathInfo info = resolvePath(path);
    if (!info.found) return LFS_ENOENT;
    
    strncpy(levelName, info.level.c_str(), size - 1);
    levelName[size - 1] = 0;
    return LFS_OK;
}

void vfsSetGlobals(IDiskReader* disk, VfsSuperBlock* sb, VfsNavigationContext* ctx) {
    vfsDiskPtr = disk;
    vfsSbPtr = sb;
    vfsContextPtr = ctx;
}
