/*
 * vfs_types.hpp - Minimal LevelFS Types for VFS Layer
 *
 * Contains only the structures and constants needed by VFS,
 * without Windows headers to avoid C++17 std::byte conflicts.
 */

#ifndef VFS_TYPES_HPP
#define VFS_TYPES_HPP

#include <cstdint>
#include <cstring>

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif
#ifndef CLUSTER_SIZE
#define CLUSTER_SIZE 4096 
#endif
#ifndef SECTORS_PER_CLUSTER
#define SECTORS_PER_CLUSTER 8
#endif

#ifndef LAT_FREE
#define LAT_FREE 0x0000000000000000ULL
#endif
#ifndef LAT_END
#define LAT_END  0xFFFFFFFFFFFFFFFFULL
#endif
#ifndef LAT_BAD
#define LAT_BAD  0xFFFFFFFFFFFFFFFEULL
#endif
#ifndef LIT_EMPTY
#define LIT_EMPTY 0x0000000000000000ULL
#endif

#ifndef LAB_ENTRIES_PER_CLUSTER
#define LAB_ENTRIES_PER_CLUSTER 256
#endif
#ifndef LIT_ENTRIES_PER_CLUSTER
#define LIT_ENTRIES_PER_CLUSTER 512
#endif
#ifndef CLUSTERS_PER_LIT_ENTRY
#define CLUSTERS_PER_LIT_ENTRY 256
#endif

#ifndef TYPE_FREE
#define TYPE_FREE 0
#endif
#ifndef TYPE_FILE
#define TYPE_FILE 1
#endif
#ifndef TYPE_LEVELED_DIR
#define TYPE_LEVELED_DIR 2
#endif
#ifndef TYPE_SYMLINK
#define TYPE_SYMLINK 3
#endif
#ifndef TYPE_HARDLINK
#define TYPE_HARDLINK 4
#endif
#ifndef TYPE_LEVEL_MOUNT
#define TYPE_LEVEL_MOUNT 5
#endif

#ifndef PERM_READ
#define PERM_READ     0x0001
#endif
#ifndef PERM_WRITE
#define PERM_WRITE    0x0002
#endif
#ifndef PERM_EXEC
#define PERM_EXEC     0x0004
#endif

#pragma pack(push, 1)

struct VfsSuperBlock {
    uint32_t magic;
    uint32_t version;
    uint64_t totalSectors;
    uint32_t clusterSize;
    uint64_t totalClusters;
    uint64_t litStartCluster;
    uint64_t litClusters;
    uint64_t labPoolStart;
    uint64_t labPoolClusters;
    uint64_t nextFreeLAB;
    uint64_t levelRegistryCluster;
    uint64_t levelRegistryClusters;
    uint64_t journalStartCluster;
    uint64_t journalSectors;
    uint64_t lastTxId;
    uint64_t nextLevelID;
    uint64_t totalLevels;
    uint64_t rootLevelID;
    uint64_t rootDirCluster;
    uint64_t backupSBCluster;
    uint64_t freeClusterHint;
    uint64_t totalFreeClusters;
    uint64_t latStartCluster;
    uint64_t latSectors;
    char volumeName[32];
    char padding[312];
};

struct VfsLITEntry {
    uint64_t labCluster;
    uint64_t baseCluster;
    uint32_t allocatedCount;
    uint32_t flags;
};

struct VfsLABEntry {
    uint64_t nextCluster;
    uint32_t levelID;
    uint16_t flags;
    uint16_t refCount;
};

struct VfsDirEntry {
    char name[24];
    char extension[4];
    uint8_t type;
    uint8_t ownerLevel;
    uint8_t reserved[2];
    uint64_t startCluster;
    uint64_t size;
    uint32_t attributes;
    uint32_t createTime;
    uint32_t modTime;
    uint32_t accessTime;
};

struct VfsVersionEntry {
    char versionName[32];
    uint64_t contentTableCluster;
    uint64_t levelID;
    uint64_t parentLevelID;
    uint32_t flags;
    uint32_t permissions;
    uint32_t createTime;
    uint32_t modTime;
    uint8_t isActive;
    uint8_t isLocked;
    uint8_t isSnapshot;
    char padding[1];
};

#pragma pack(pop)

class IDiskReader {
public:
    virtual ~IDiskReader() = default;
    virtual bool isOpen() const = 0;
    virtual bool readSector(uint64_t sector, void* buffer, uint32_t count = 1) = 0;
    virtual bool writeSector(uint64_t sector, const void* buffer, uint32_t count = 1) = 0;
};

struct VfsNavigationContext {
    uint64_t currentDirCluster;
    uint64_t currentContentCluster;
    uint64_t rootContentCluster;
    uint64_t currentLevelID;
    uint64_t rootLevelID;
    uint32_t currentFolderPerms;
    char currentPath[260];
    char currentVersion[32];
    char rootVersion[32];

    VfsNavigationContext() {
        currentDirCluster = 0;
        currentContentCluster = 0;
        rootContentCluster = 0;
        currentLevelID = 0;
        rootLevelID = 0;
        currentFolderPerms = 0;
        memset(currentPath, 0, sizeof(currentPath));
        memset(currentVersion, 0, sizeof(currentVersion));
        memset(rootVersion, 0, sizeof(rootVersion));
    }
};

#endif
