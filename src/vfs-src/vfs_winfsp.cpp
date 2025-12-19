/*
 * vfs_winfsp.cpp - WinFsp Integration for LevelFS VFS (FUSE-compatible layer)
 *
 * Compile: g++ -c vfs_winfsp.cpp -o vfs_winfsp.o -std=c++17 -DFUSE_USE_VERSION=26 -I<winfsp-include>/fuse
 * Link: -lwinfsp-x64 or link with winfsp-x64.dll
 *
 * Uses WinFsp's FUSE compatibility layer for better MinGW support.
 */

#ifdef _WIN32

#define FUSE_USE_VERSION 26

#include "vfs_platform.hpp"
#include "levelfs_vfs.hpp"

#include <fuse.h>
#include <string>
#include <mutex>
#include <memory>

using namespace std;

static IVfsOperations* g_ops = nullptr;
static mutex g_opsMutex;

static int lfs_fuse_getattr(const char* path, struct fuse_stat* stbuf) {
    lock_guard<mutex> lock(g_opsMutex);
    if (!g_ops) return -ENOENT;
    
    memset(stbuf, 0, sizeof(struct fuse_stat));
    
    LfsStat st;
    LfsError err = g_ops->getattr(path, &st);
    if (err != LFS_OK) {
        switch (err) {
            case LFS_ENOENT: return -ENOENT;
            case LFS_EACCES: return -EACCES;
            case LFS_EIO: return -EIO;
            default: return -EIO;
        }
    }
    
    stbuf->st_ino = st.ino;
    stbuf->st_mode = st.mode;
    stbuf->st_nlink = st.nlink;
    stbuf->st_size = st.size;
    stbuf->st_atime = st.atime;
    stbuf->st_mtime = st.mtime;
    stbuf->st_ctime = st.ctime;
    
    return 0;
}

static int lfs_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                            fuse_off_t offset, struct fuse_file_info* fi) {
    lock_guard<mutex> lock(g_opsMutex);
    if (!g_ops) return -ENOENT;
    
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    
    auto callback = [&](const char* name, const LfsStat* st) -> int {
        struct fuse_stat stbuf;
        memset(&stbuf, 0, sizeof(stbuf));
        stbuf.st_ino = st->ino;
        stbuf.st_mode = st->mode;
        stbuf.st_size = st->size;
        filler(buf, name, &stbuf, 0);
        return 0;
    };
    
    LfsError err = g_ops->readdir(path, callback);
    if (err != LFS_OK) return -EIO;
    
    return 0;
}

static int lfs_fuse_open(const char* path, struct fuse_file_info* fi) {
    lock_guard<mutex> lock(g_opsMutex);
    if (!g_ops) return -ENOENT;
    
    LfsHandle fh;
    uint32_t flags = 0;
    if ((fi->flags & O_ACCMODE) == O_RDONLY) flags = LFS_O_RDONLY;
    else if ((fi->flags & O_ACCMODE) == O_WRONLY) flags = LFS_O_WRONLY;
    else if ((fi->flags & O_ACCMODE) == O_RDWR) flags = LFS_O_RDWR;
    
    LfsError err = g_ops->open(path, flags, &fh);
    if (err != LFS_OK) {
        switch (err) {
            case LFS_ENOENT: return -ENOENT;
            case LFS_EACCES: return -EACCES;
            default: return -EIO;
        }
    }
    
    fi->fh = fh.id;
    return 0;
}

static int lfs_fuse_read(const char* path, char* buf, size_t size,
                         fuse_off_t offset, struct fuse_file_info* fi) {
    lock_guard<mutex> lock(g_opsMutex);
    if (!g_ops) return -ENOENT;
    
    LfsHandle fh;
    fh.id = fi->fh;
    
    size_t bytesRead = 0;
    LfsError err = g_ops->read(&fh, buf, size, offset, &bytesRead);
    if (err != LFS_OK) return -EIO;
    
    return (int)bytesRead;
}

static int lfs_fuse_release(const char* path, struct fuse_file_info* fi) {
    lock_guard<mutex> lock(g_opsMutex);
    if (!g_ops) return 0;
    
    LfsHandle fh;
    fh.id = fi->fh;
    g_ops->release(&fh);
    return 0;
}

static int lfs_fuse_statfs(const char* path, struct fuse_statvfs* stbuf) {
    lock_guard<mutex> lock(g_opsMutex);
    if (!g_ops) return -EIO;
    
    memset(stbuf, 0, sizeof(struct fuse_statvfs));
    
    LfsStatFs st;
    LfsError err = g_ops->statfs(path, &st);
    if (err != LFS_OK) return -EIO;
    
    stbuf->f_bsize = st.blockSize;
    stbuf->f_blocks = st.totalBlocks;
    stbuf->f_bfree = st.freeBlocks;
    stbuf->f_bavail = st.freeBlocks;
    stbuf->f_namemax = st.maxNameLen;
    
    return 0;
}

static struct fuse_operations lfs_fuse_ops = {
    .getattr = lfs_fuse_getattr,
    .open = lfs_fuse_open,
    .read = lfs_fuse_read,
    .statfs = lfs_fuse_statfs,
    .release = lfs_fuse_release,
    .readdir = lfs_fuse_readdir,
};

class WinFspFusePlatform : public IVfsPlatform {
private:
    struct fuse* fuseInstance;
    struct fuse_chan* fuseChan;
    string mountPoint;
    bool mounted;
    int lastError;
    string lastErrorStr;
    
public:
    WinFspFusePlatform() : fuseInstance(nullptr), fuseChan(nullptr), 
                           mounted(false), lastError(0) {}
    
    ~WinFspFusePlatform() override {
        unmount();
    }
    
    VfsPlatformType getType() const override { return VfsPlatformType::WINFSP; }
    const char* getName() const override { return "WinFsp-FUSE"; }
    
    int mount(const VfsMountOptions& options, IVfsOperations* ops) override {
        if (mounted) {
            lastError = -1;
            lastErrorStr = "Already mounted";
            return -1;
        }
        
        g_ops = ops;
        mountPoint = options.mountPoint;
        
        char* argv[] = {
            (char*)"levelfs",
            (char*)"-f",
            (char*)"-s",
            (char*)mountPoint.c_str(),
            nullptr
        };
        int argc = 4;
        
        struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
        
        fuseChan = fuse_mount(mountPoint.c_str(), &args);
        if (!fuseChan) {
            lastError = -1;
            lastErrorStr = "fuse_mount failed";
            fuse_opt_free_args(&args);
            return -1;
        }
        
        fuseInstance = fuse_new(fuseChan, &args, &lfs_fuse_ops, sizeof(lfs_fuse_ops), NULL);
        fuse_opt_free_args(&args);
        
        if (!fuseInstance) {
            fuse_unmount(mountPoint.c_str(), fuseChan);
            fuseChan = nullptr;
            lastError = -1;
            lastErrorStr = "fuse_new failed";
            return -1;
        }
        
        mounted = true;
        return 0;
    }
    
    int unmount() override {
        if (!mounted) return 0;
        
        if (fuseInstance) {
            fuse_exit(fuseInstance);
            fuse_destroy(fuseInstance);
            fuseInstance = nullptr;
        }
        
        if (fuseChan) {
            fuse_unmount(mountPoint.c_str(), fuseChan);
            fuseChan = nullptr;
        }
        
        g_ops = nullptr;
        mounted = false;
        return 0;
    }
    
    bool isMounted() const override { return mounted; }
    string getMountPoint() const override { return mountPoint; }
    
    void runLoop() override {
        if (fuseInstance) {
            fuse_loop(fuseInstance);
        }
    }
    
    int getLastError() const override { return lastError; }
    string getLastErrorString() const override { return lastErrorStr; }
};

IVfsPlatform* createWinFspPlatform() {
    return new WinFspFusePlatform();
}

#else

IVfsPlatform* createWinFspPlatform() {
    return nullptr;
}

#endif

IVfsPlatform* createFusePlatform() {
    return nullptr;
}
