/*
 * vfs_platform.hpp - Platform Abstraction for VFS Mounting
 *
 * Compile: Include with #include "vfs_platform.hpp"
 *
 * Abstract interface for platform-specific VFS mounting (WinFsp, FUSE, etc.)
 */

#ifndef VFS_PLATFORM_HPP
#define VFS_PLATFORM_HPP

#include "vfs_interface.hpp"
#include <string>

using namespace std;

enum class VfsPlatformType {
    WINFSP,
    FUSE,
    DOKAN,
    UNKNOWN
};

struct VfsMountOptions {
    string mountPoint;
    string volumeName;
    string fileSystemName;
    bool readOnly;
    bool debugMode;
    uint32_t sectorSize;
    uint32_t sectorsPerCluster;
    uint64_t volumeSize;
    uint32_t threadCount;
    
    VfsMountOptions() : readOnly(false), debugMode(false),
                        sectorSize(512), sectorsPerCluster(8),
                        volumeSize(0), threadCount(0) {
        fileSystemName = "LevelFS";
    }
};

class IVfsPlatform {
public:
    virtual ~IVfsPlatform() = default;

    virtual VfsPlatformType getType() const = 0;
    virtual const char* getName() const = 0;

    virtual int mount(const VfsMountOptions& options, IVfsOperations* ops) = 0;
    virtual int unmount() = 0;

    virtual bool isMounted() const = 0;
    virtual string getMountPoint() const = 0;

    virtual void runLoop() = 0;

    virtual int getLastError() const = 0;
    virtual string getLastErrorString() const = 0;
};

IVfsPlatform* createWinFspPlatform();
IVfsPlatform* createFusePlatform();

inline IVfsPlatform* createPlatform(VfsPlatformType type) {
    switch (type) {
        case VfsPlatformType::WINFSP:
            return createWinFspPlatform();
        case VfsPlatformType::FUSE:
            return createFusePlatform();
        default:
            return nullptr;
    }
}

#ifdef _WIN32
inline IVfsPlatform* createDefaultPlatform() {
    return createWinFspPlatform();
}
#else
inline IVfsPlatform* createDefaultPlatform() {
    return createFusePlatform();
}
#endif

#endif
