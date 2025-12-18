/*
 * vfs.cpp - Virtual File System Layer Implementation for LevelFS
 * 
 * Compile: g++ vfs.cpp -c -o vfs.o -std=c++17
 */

#include "vfs.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>

LevelVFS::LevelVFS() 
    : journal(nullptr), entryReader(nullptr), entryWriter(nullptr), 
      entryFinder(nullptr), mounted(false), nextHandle(1) {
    memset(&sb, 0, sizeof(sb));
}

LevelVFS::~LevelVFS() {
    unmount();
}

void LevelVFS::unmount() {
    if (!mounted) return;
    
    for (auto& kv : openFiles) {
        close(kv.first);
    }
    openFiles.clear();
    
    if (journal) { delete journal; journal = nullptr; }
    if (entryReader) { delete entryReader; entryReader = nullptr; }
    if (entryWriter) { delete entryWriter; entryWriter = nullptr; }
    if (entryFinder) { delete entryFinder; entryFinder = nullptr; }
    
    disk.close();
    mounted = false;
    mountedPath.clear();
}

int LevelVFS::mount(char driveLetter) {
    LFS_ENSURE(!mounted, LFS_ERR_BUSY);
    
    if (!disk.open(driveLetter)) {
        return LFS_ERR_DISK_IO;
    }
    
    if (!disk.readSector(0, &sb)) {
        disk.close();
        return LFS_ERR_DISK_IO;
    }
    
    if (sb.magic != MAGIC) {
        if (!tryBackupSuperblock()) {
            disk.close();
            return LFS_ERR_CORRUPT;
        }
    }
    
    if (sb.version != LFS_VERSION) {
        disk.close();
        return LFS_ERR_INVALID;
    }
    
    entryReader = new EntryReader(disk);
    entryWriter = new EntryWriter(disk);
    entryFinder = new EntryFinder(disk);
    journal = new Journal(&disk, &sb);
    
    lockManager.initSystemLock(to_string(sb.magic));
    
    context.currentLevelID = sb.rootLevelID;
    context.currentContentCluster = sb.rootDirCluster + 1;
    context.rootContentCluster = context.currentContentCluster;
    
    mounted = true;
    mountedPath = string(1, driveLetter) + ":";
    
    return LFS_SUCCESS;
}

int LevelVFS::mountAuto() {
    LFS_ENSURE(!mounted, LFS_ERR_BUSY);
    
    for (int diskIdx = 0; diskIdx < 16; diskIdx++) {
        string path = "\\\\.\\PhysicalDrive" + to_string(diskIdx);
        HANDLE hDevice = CreateFileA(path.c_str(), GENERIC_READ, 
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) continue;
        
        const int layoutSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 
            sizeof(PARTITION_INFORMATION_EX) * 128;
        vector<char> layoutBuf(layoutSize);
        DRIVE_LAYOUT_INFORMATION_EX* layout = (DRIVE_LAYOUT_INFORMATION_EX*)layoutBuf.data();
        DWORD bytesRet;
        
        if (DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, 
            layout, layoutSize, &bytesRet, NULL)) {
            for (DWORD i = 0; i < layout->PartitionCount; i++) {
                PARTITION_INFORMATION_EX* p = &layout->PartitionEntry[i];
                if (p->PartitionLength.QuadPart == 0) continue;
                
                int result = mountDirect(diskIdx, p->StartingOffset.QuadPart);
                if (result == LFS_SUCCESS) {
                    CloseHandle(hDevice);
                    return LFS_SUCCESS;
                }
            }
        }
        CloseHandle(hDevice);
    }
    
    return LFS_ERR_NOT_FOUND;
}

int LevelVFS::mountImage(const string& path) {
    LFS_ENSURE(!mounted, LFS_ERR_BUSY);
    
    if (!disk.openFile(path)) {
        return LFS_ERR_DISK_IO;
    }
    
    if (!disk.readSector(0, &sb)) {
        disk.close();
        return LFS_ERR_DISK_IO;
    }
    
    if (sb.magic != MAGIC) {
        disk.close();
        return LFS_ERR_CORRUPT;
    }
    
    entryReader = new EntryReader(disk);
    entryWriter = new EntryWriter(disk);
    entryFinder = new EntryFinder(disk);
    journal = new Journal(&disk, &sb);
    
    context.currentLevelID = sb.rootLevelID;
    context.currentContentCluster = sb.rootDirCluster + 1;
    context.rootContentCluster = context.currentContentCluster;
    
    mounted = true;
    mountedPath = path;
    
    return LFS_SUCCESS;
}

int LevelVFS::mountDirect(int diskIndex, uint64_t offset) {
    LFS_ENSURE(!mounted, LFS_ERR_BUSY);
    
    string path = "\\\\.\\PhysicalDrive" + to_string(diskIndex);
    if (!disk.open(path, offset)) {
        return LFS_ERR_DISK_IO;
    }
    
    if (!disk.readSector(0, &sb)) {
        disk.close();
        return LFS_ERR_DISK_IO;
    }
    
    if (sb.magic != MAGIC) {
        disk.close();
        return LFS_ERR_CORRUPT;
    }
    
    entryReader = new EntryReader(disk);
    entryWriter = new EntryWriter(disk);
    entryFinder = new EntryFinder(disk);
    journal = new Journal(&disk, &sb);
    
    context.currentLevelID = sb.rootLevelID;
    context.currentContentCluster = sb.rootDirCluster + 1;
    context.rootContentCluster = context.currentContentCluster;
    
    mounted = true;
    mountedPath = path + "+" + to_string(offset);
    
    return LFS_SUCCESS;
}

bool LevelVFS::tryBackupSuperblock() {
    if (sb.backupSBCluster == 0) return false;
    
    SuperBlock backup;
    if (!disk.readSector(sb.backupSBCluster * SECTORS_PER_CLUSTER, &backup)) {
        return false;
    }
    
    if (backup.magic != MAGIC) return false;
    
    sb = backup;
    writeSuperBlock();
    return true;
}

void LevelVFS::writeSuperBlock() {
    disk.writeSector(0, &sb);
    if (sb.backupSBCluster > 0) {
        disk.writeSector(sb.backupSBCluster * SECTORS_PER_CLUSTER, &sb);
    }
}

LABEntry LevelVFS::getLABEntry(uint64_t cluster) {
    LABEntry buffer[LAB_ENTRIES_PER_CLUSTER];
    uint64_t sector = cluster / LAB_ENTRIES_PER_CLUSTER;
    uint64_t offset = cluster % LAB_ENTRIES_PER_CLUSTER;
    disk.readSector(sector, buffer);
    return buffer[offset];
}

void LevelVFS::setLABEntry(uint64_t cluster, LABEntry value) {
    LABEntry buffer[LAB_ENTRIES_PER_CLUSTER];
    uint64_t sector = cluster / LAB_ENTRIES_PER_CLUSTER;
    uint64_t offset = cluster % LAB_ENTRIES_PER_CLUSTER;
    disk.readSector(sector, buffer);
    buffer[offset] = value;
    disk.writeSector(sector, buffer);
}

bool LevelVFS::isReservedCluster(uint64_t cluster) {
    if (cluster == 0) return true;
    if (cluster < sb.litStartCluster + sb.litClusters) return true;
    if (cluster >= sb.labPoolStart && cluster < sb.labPoolStart + sb.labPoolClusters) return true;
    if (cluster >= sb.levelRegistryCluster && cluster < sb.levelRegistryCluster + sb.levelRegistryClusters) return true;
    if (cluster >= sb.journalStartCluster && cluster < sb.journalStartCluster + (sb.journalSectors / SECTORS_PER_CLUSTER) + 1) return true;
    if (cluster >= sb.rootDirCluster && cluster <= sb.rootDirCluster + 1) return true;
    if (cluster == sb.backupSBCluster) return true;
    return false;
}

uint64_t LevelVFS::allocCluster() {
    return allocClusterForLevel(context.currentLevelID);
}

uint64_t LevelVFS::allocClusterForLevel(uint32_t levelID) {
    uint64_t start = sb.freeClusterHint;
    if (start < sb.labPoolStart + sb.labPoolClusters) {
        start = sb.labPoolStart + sb.labPoolClusters;
    }
    
    for (uint64_t c = start; c < sb.totalClusters; c++) {
        if (isReservedCluster(c)) continue;
        
        LABEntry lab = getLABEntry(c);
        if (lab.nextCluster == LAT_FREE) {
            LABEntry newEntry;
            newEntry.nextCluster = LAT_END;
            newEntry.levelID = levelID;
            newEntry.flags = 0;
            newEntry.refCount = 1;
            setLABEntry(c, newEntry);
            
            sb.freeClusterHint = c + 1;
            sb.totalFreeClusters--;
            writeSuperBlock();
            return c;
        }
    }
    
    for (uint64_t c = sb.labPoolStart + sb.labPoolClusters; c < start; c++) {
        if (isReservedCluster(c)) continue;
        
        LABEntry lab = getLABEntry(c);
        if (lab.nextCluster == LAT_FREE) {
            LABEntry newEntry;
            newEntry.nextCluster = LAT_END;
            newEntry.levelID = levelID;
            newEntry.flags = 0;
            newEntry.refCount = 1;
            setLABEntry(c, newEntry);
            
            sb.freeClusterHint = c + 1;
            sb.totalFreeClusters--;
            writeSuperBlock();
            return c;
        }
    }
    
    return 0;
}

void LevelVFS::freeCluster(uint64_t cluster) {
    if (cluster == 0 || isReservedCluster(cluster)) return;
    
    LABEntry entry;
    entry.nextCluster = LAT_FREE;
    entry.levelID = LEVEL_ID_NONE;
    entry.flags = 0;
    entry.refCount = 0;
    setLABEntry(cluster, entry);
    
    sb.totalFreeClusters++;
    if (cluster < sb.freeClusterHint) {
        sb.freeClusterHint = cluster;
    }
    writeSuperBlock();
}

void LevelVFS::freeChain(uint64_t startCluster) {
    if (startCluster == 0) return;
    
    vector<uint64_t> chain = getChain(startCluster);
    for (uint64_t c : chain) {
        freeCluster(c);
    }
}

vector<uint64_t> LevelVFS::getChain(uint64_t startCluster) {
    vector<uint64_t> chain;
    if (startCluster == 0) return chain;
    
    uint64_t current = startCluster;
    int limit = 100000;
    
    while (current != 0 && current != LAT_END && current != LAT_FREE && limit-- > 0) {
        chain.push_back(current);
        LABEntry lab = getLABEntry(current);
        if (lab.nextCluster == current) break;
        current = lab.nextCluster;
    }
    
    return chain;
}

int LevelVFS::readClusterData(uint64_t cluster, void* buffer) {
    for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
        if (!disk.readSector(cluster * SECTORS_PER_CLUSTER + s, 
            (char*)buffer + s * SECTOR_SIZE)) {
            return LFS_ERR_DISK_IO;
        }
    }
    return LFS_SUCCESS;
}

int LevelVFS::writeClusterData(uint64_t cluster, const void* buffer) {
    for (int s = 0; s < SECTORS_PER_CLUSTER; s++) {
        if (!disk.writeSector(cluster * SECTORS_PER_CLUSTER + s,
            (const char*)buffer + s * SECTOR_SIZE)) {
            return LFS_ERR_DISK_IO;
        }
    }
    return LFS_SUCCESS;
}

LevelVFS::PathResult LevelVFS::resolvePath(const string& path) {
    PathResult result;
    result.valid = false;
    
    if (path.empty()) {
        result.parentCluster = context.currentContentCluster;
        result.name = "";
        result.valid = true;
        return result;
    }
    
    string cleanPath = path;
    if (cleanPath[0] == '/') cleanPath = cleanPath.substr(1);
    if (!cleanPath.empty() && cleanPath.back() == '/') cleanPath.pop_back();
    
    if (cleanPath.empty()) {
        result.parentCluster = context.currentContentCluster;
        result.name = "";
        result.valid = true;
        return result;
    }
    
    vector<string> parts;
    size_t pos = 0;
    while ((pos = cleanPath.find('/')) != string::npos) {
        string part = cleanPath.substr(0, pos);
        if (!part.empty()) parts.push_back(part);
        cleanPath = cleanPath.substr(pos + 1);
    }
    if (!cleanPath.empty()) parts.push_back(cleanPath);
    
    if (parts.empty()) {
        result.parentCluster = context.currentContentCluster;
        result.name = "";
        result.valid = true;
        return result;
    }
    
    result.name = parts.back();
    parts.pop_back();
    
    uint64_t current = context.currentContentCluster;
    
    for (const string& part : parts) {
        FindResult found = findEntry(current, part);
        if (!found.found) {
            return result;
        }
        
        if (found.entry.type == TYPE_LEVELED_DIR) {
            VersionEntry versions[CLUSTER_SIZE / sizeof(VersionEntry)];
            readClusterData(found.entry.startCluster, versions);
            
            bool foundActive = false;
            for (int i = 0; i < CLUSTER_SIZE / sizeof(VersionEntry); i++) {
                if (versions[i].isActive && versions[i].levelID == context.currentLevelID) {
                    current = versions[i].contentTableCluster;
                    foundActive = true;
                    break;
                }
            }
            if (!foundActive) {
                for (int i = 0; i < CLUSTER_SIZE / sizeof(VersionEntry); i++) {
                    if (versions[i].isActive) {
                        current = versions[i].contentTableCluster;
                        foundActive = true;
                        break;
                    }
                }
            }
            if (!foundActive) return result;
        } else {
            return result;
        }
    }
    
    result.parentCluster = current;
    result.valid = true;
    return result;
}

FindResult LevelVFS::findEntry(uint64_t contentCluster, const string& name) {
    return entryFinder->findByName(contentCluster, name);
}

LfsResultValue<VFSHandle> LevelVFS::open(const string& path, int mode) {
    LFS_ENSURE_MOUNTED();
    
    PathResult pr = resolvePath(path);
    if (!pr.valid) {
        return LfsResultValue<VFSHandle>(LFS_ERR_NOT_FOUND);
    }
    
    FindResult found = findEntry(pr.parentCluster, pr.name);
    
    if (!found.found) {
        if (!(mode & VFS_MODE_CREATE)) {
            return LfsResultValue<VFSHandle>(LFS_ERR_NOT_FOUND);
        }
        
        int createResult = touch(path);
        if (createResult != LFS_SUCCESS) {
            return LfsResultValue<VFSHandle>(createResult);
        }
        
        found = findEntry(pr.parentCluster, pr.name);
        if (!found.found) {
            return LfsResultValue<VFSHandle>(LFS_ERR_GENERAL);
        }
    }
    
    if (found.entry.type != TYPE_FILE) {
        return LfsResultValue<VFSHandle>(LFS_ERR_IS_DIR);
    }
    
    if ((mode & VFS_MODE_WRITE) && !(found.entry.attributes & PERM_WRITE)) {
        return LfsResultValue<VFSHandle>(LFS_ERR_PERMISSION);
    }
    
    if ((mode & VFS_MODE_READ) && !(found.entry.attributes & PERM_READ)) {
        return LfsResultValue<VFSHandle>(LFS_ERR_PERMISSION);
    }
    
    VFSOpenFile of;
    of.handle = nextHandle++;
    of.startCluster = found.entry.startCluster;
    of.size = found.entry.size;
    of.position = (mode & VFS_MODE_APPEND) ? found.entry.size : 0;
    of.mode = mode;
    of.path = path;
    of.valid = true;
    
    if (mode & VFS_MODE_TRUNC) {
        if (found.entry.startCluster != 0) {
            freeChain(found.entry.startCluster);
        }
        found.entry.startCluster = 0;
        found.entry.size = 0;
        entryWriter->writeEntry(found.location.cluster, found.location.sector, 
            found.location.index, found.entry);
        of.startCluster = 0;
        of.size = 0;
    }
    
    openFiles[of.handle] = of;
    return LfsResultValue<VFSHandle>(of.handle);
}

int LevelVFS::read(VFSHandle handle, void* buffer, size_t size) {
    LFS_ENSURE_MOUNTED();
    
    auto it = openFiles.find(handle);
    if (it == openFiles.end() || !it->second.valid) {
        return LFS_ERR_INVALID;
    }
    
    VFSOpenFile& of = it->second;
    
    if (!(of.mode & VFS_MODE_READ)) {
        return LFS_ERR_PERMISSION;
    }
    
    if (of.position >= of.size) {
        return 0;
    }
    
    size_t toRead = min(size, (size_t)(of.size - of.position));
    if (toRead == 0) return 0;
    
    vector<uint64_t> chain = getChain(of.startCluster);
    if (chain.empty()) return 0;
    
    size_t bytesRead = 0;
    char* dst = (char*)buffer;
    
    while (bytesRead < toRead && of.position < of.size) {
        uint64_t clusterIdx = of.position / CLUSTER_SIZE;
        uint64_t offsetInCluster = of.position % CLUSTER_SIZE;
        
        if (clusterIdx >= chain.size()) break;
        
        char clusterData[CLUSTER_SIZE];
        if (readClusterData(chain[clusterIdx], clusterData) != LFS_SUCCESS) {
            return LFS_ERR_DISK_IO;
        }
        
        size_t canRead = min(toRead - bytesRead, (size_t)(CLUSTER_SIZE - offsetInCluster));
        canRead = min(canRead, (size_t)(of.size - of.position));
        
        memcpy(dst + bytesRead, clusterData + offsetInCluster, canRead);
        bytesRead += canRead;
        of.position += canRead;
    }
    
    return (int)bytesRead;
}

int LevelVFS::write(VFSHandle handle, const void* buffer, size_t size) {
    LFS_ENSURE_MOUNTED();
    
    auto it = openFiles.find(handle);
    if (it == openFiles.end() || !it->second.valid) {
        return LFS_ERR_INVALID;
    }
    
    VFSOpenFile& of = it->second;
    
    if (!(of.mode & VFS_MODE_WRITE)) {
        return LFS_ERR_PERMISSION;
    }
    
    if (size == 0) return 0;
    
    vector<uint64_t> chain = getChain(of.startCluster);
    
    const char* src = (const char*)buffer;
    size_t bytesWritten = 0;
    
    while (bytesWritten < size) {
        uint64_t clusterIdx = of.position / CLUSTER_SIZE;
        uint64_t offsetInCluster = of.position % CLUSTER_SIZE;
        
        while (clusterIdx >= chain.size()) {
            uint64_t newCluster = allocCluster();
            if (newCluster == 0) {
                return LFS_ERR_NO_SPACE;
            }
            
            if (chain.empty()) {
                of.startCluster = newCluster;
            } else {
                LABEntry prev = getLABEntry(chain.back());
                prev.nextCluster = newCluster;
                setLABEntry(chain.back(), prev);
            }
            chain.push_back(newCluster);
        }
        
        char clusterData[CLUSTER_SIZE];
        memset(clusterData, 0, CLUSTER_SIZE);
        
        if (offsetInCluster > 0 || size - bytesWritten < CLUSTER_SIZE) {
            readClusterData(chain[clusterIdx], clusterData);
        }
        
        size_t canWrite = min(size - bytesWritten, (size_t)(CLUSTER_SIZE - offsetInCluster));
        memcpy(clusterData + offsetInCluster, src + bytesWritten, canWrite);
        
        if (writeClusterData(chain[clusterIdx], clusterData) != LFS_SUCCESS) {
            return LFS_ERR_DISK_IO;
        }
        
        bytesWritten += canWrite;
        of.position += canWrite;
        
        if (of.position > of.size) {
            of.size = of.position;
        }
    }
    
    PathResult pr = resolvePath(of.path);
    if (pr.valid) {
        FindResult found = findEntry(pr.parentCluster, pr.name);
        if (found.found) {
            found.entry.size = of.size;
            found.entry.startCluster = of.startCluster;
            found.entry.modTime = time(nullptr);
            entryWriter->writeEntry(found.location.cluster, found.location.sector,
                found.location.index, found.entry);
        }
    }
    
    return (int)bytesWritten;
}

int LevelVFS::seek(VFSHandle handle, int64_t offset, VFSSeekMode whence) {
    LFS_ENSURE_MOUNTED();
    
    auto it = openFiles.find(handle);
    if (it == openFiles.end() || !it->second.valid) {
        return LFS_ERR_INVALID;
    }
    
    VFSOpenFile& of = it->second;
    int64_t newPos;
    
    switch (whence) {
        case VFS_SEEK_SET: newPos = offset; break;
        case VFS_SEEK_CUR: newPos = (int64_t)of.position + offset; break;
        case VFS_SEEK_END: newPos = (int64_t)of.size + offset; break;
        default: return LFS_ERR_INVALID;
    }
    
    if (newPos < 0) newPos = 0;
    of.position = (uint64_t)newPos;
    
    return LFS_SUCCESS;
}

int64_t LevelVFS::tell(VFSHandle handle) {
    if (!mounted) return -1;
    
    auto it = openFiles.find(handle);
    if (it == openFiles.end() || !it->second.valid) {
        return -1;
    }
    
    return (int64_t)it->second.position;
}

int LevelVFS::close(VFSHandle handle) {
    LFS_ENSURE_MOUNTED();
    
    auto it = openFiles.find(handle);
    if (it == openFiles.end()) {
        return LFS_ERR_INVALID;
    }
    
    openFiles.erase(it);
    return LFS_SUCCESS;
}

int LevelVFS::touch(const string& path, const string& extension) {
    LFS_ENSURE_MOUNTED();
    
    PathResult pr = resolvePath(path);
    if (!pr.valid || pr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult existing = findEntry(pr.parentCluster, pr.name);
    if (existing.found) {
        return LFS_ERR_EXISTS;
    }
    
    EntryLocation slot = entryFinder->findFreeSlot(pr.parentCluster);
    if (!slot.found) {
        return LFS_ERR_NO_SPACE;
    }
    
    DirEntry newEntry;
    memset(&newEntry, 0, sizeof(newEntry));
    newEntry.type = TYPE_FILE;
    strncpy(newEntry.name, pr.name.c_str(), 24);
    if (!extension.empty()) {
        strncpy(newEntry.extension, extension.c_str(), 8);
    }
    newEntry.attributes = PERM_READ | PERM_WRITE;
    newEntry.createTime = time(nullptr);
    newEntry.modTime = newEntry.createTime;
    newEntry.size = 0;
    
    if (!entryWriter->writeEntry(slot.cluster, slot.sector, slot.index, newEntry)) {
        return LFS_ERR_DISK_IO;
    }
    
    return LFS_SUCCESS;
}

int LevelVFS::mkdir(const string& path) {
    LFS_ENSURE_MOUNTED();
    
    PathResult pr = resolvePath(path);
    if (!pr.valid || pr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult existing = findEntry(pr.parentCluster, pr.name);
    if (existing.found) {
        return LFS_ERR_EXISTS;
    }
    
    uint64_t versionCluster = allocCluster();
    if (versionCluster == 0) return LFS_ERR_NO_SPACE;
    
    uint64_t contentCluster = allocCluster();
    if (contentCluster == 0) {
        freeCluster(versionCluster);
        return LFS_ERR_NO_SPACE;
    }
    
    VersionEntry versions[CLUSTER_SIZE / sizeof(VersionEntry)];
    memset(versions, 0, sizeof(versions));
    strcpy(versions[0].versionName, "master");
    versions[0].isActive = 1;
    versions[0].contentTableCluster = contentCluster;
    versions[0].levelID = context.currentLevelID;
    versions[0].parentLevelID = LEVEL_ID_NONE;
    versions[0].flags = LEVEL_FLAG_ACTIVE;
    versions[0].permissions = PERM_READ | PERM_WRITE | PERM_EXEC;
    versions[0].createTime = time(nullptr);
    versions[0].modTime = versions[0].createTime;
    
    if (writeClusterData(versionCluster, versions) != LFS_SUCCESS) {
        freeCluster(versionCluster);
        freeCluster(contentCluster);
        return LFS_ERR_DISK_IO;
    }
    
    DirEntry emptyContent[CLUSTER_SIZE / sizeof(DirEntry)];
    memset(emptyContent, 0, sizeof(emptyContent));
    for (int i = 0; i < CLUSTER_SIZE / sizeof(DirEntry); i++) {
        emptyContent[i].type = TYPE_FREE;
    }
    
    if (writeClusterData(contentCluster, emptyContent) != LFS_SUCCESS) {
        freeCluster(versionCluster);
        freeCluster(contentCluster);
        return LFS_ERR_DISK_IO;
    }
    
    EntryLocation slot = entryFinder->findFreeSlot(pr.parentCluster);
    if (!slot.found) {
        freeCluster(versionCluster);
        freeCluster(contentCluster);
        return LFS_ERR_NO_SPACE;
    }
    
    DirEntry newEntry;
    memset(&newEntry, 0, sizeof(newEntry));
    newEntry.type = TYPE_LEVELED_DIR;
    strncpy(newEntry.name, pr.name.c_str(), 24);
    newEntry.attributes = PERM_READ | PERM_WRITE | PERM_EXEC;
    newEntry.createTime = time(nullptr);
    newEntry.modTime = newEntry.createTime;
    newEntry.startCluster = versionCluster;
    
    if (!entryWriter->writeEntry(slot.cluster, slot.sector, slot.index, newEntry)) {
        freeCluster(versionCluster);
        freeCluster(contentCluster);
        return LFS_ERR_DISK_IO;
    }
    
    return LFS_SUCCESS;
}

int LevelVFS::remove(const string& path) {
    LFS_ENSURE_MOUNTED();
    
    PathResult pr = resolvePath(path);
    if (!pr.valid || pr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult found = findEntry(pr.parentCluster, pr.name);
    if (!found.found) {
        return LFS_ERR_NOT_FOUND;
    }
    
    if (found.entry.type != TYPE_FILE) {
        return LFS_ERR_IS_DIR;
    }
    
    if (found.entry.startCluster != 0) {
        freeChain(found.entry.startCluster);
    }
    
    if (!entryWriter->deleteEntry(found.location.cluster, found.location.sector, 
        found.location.index)) {
        return LFS_ERR_DISK_IO;
    }
    
    return LFS_SUCCESS;
}

int LevelVFS::rmdir(const string& path) {
    LFS_ENSURE_MOUNTED();
    
    PathResult pr = resolvePath(path);
    if (!pr.valid || pr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult found = findEntry(pr.parentCluster, pr.name);
    if (!found.found) {
        return LFS_ERR_NOT_FOUND;
    }
    
    if (found.entry.type != TYPE_LEVELED_DIR) {
        return LFS_ERR_NOT_DIR;
    }
    
    VersionEntry versions[CLUSTER_SIZE / sizeof(VersionEntry)];
    if (readClusterData(found.entry.startCluster, versions) != LFS_SUCCESS) {
        return LFS_ERR_DISK_IO;
    }
    
    for (int i = 0; i < CLUSTER_SIZE / sizeof(VersionEntry); i++) {
        if (versions[i].isActive && versions[i].contentTableCluster != 0) {
            vector<DirEntry> entries = entryReader->readEntriesFromCluster(
                versions[i].contentTableCluster);
            if (!entries.empty()) {
                return LFS_ERR_NOT_EMPTY;
            }
            freeChain(versions[i].contentTableCluster);
        }
    }
    
    freeChain(found.entry.startCluster);
    
    if (!entryWriter->deleteEntry(found.location.cluster, found.location.sector,
        found.location.index)) {
        return LFS_ERR_DISK_IO;
    }
    
    return LFS_SUCCESS;
}

int LevelVFS::rename(const string& oldPath, const string& newPath) {
    LFS_ENSURE_MOUNTED();
    
    PathResult oldPr = resolvePath(oldPath);
    if (!oldPr.valid || oldPr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult found = findEntry(oldPr.parentCluster, oldPr.name);
    if (!found.found) {
        return LFS_ERR_NOT_FOUND;
    }
    
    PathResult newPr = resolvePath(newPath);
    if (!newPr.valid || newPr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult existingNew = findEntry(newPr.parentCluster, newPr.name);
    if (existingNew.found) {
        return LFS_ERR_EXISTS;
    }
    
    strncpy(found.entry.name, newPr.name.c_str(), 24);
    found.entry.modTime = time(nullptr);
    
    if (!entryWriter->writeEntry(found.location.cluster, found.location.sector,
        found.location.index, found.entry)) {
        return LFS_ERR_DISK_IO;
    }
    
    return LFS_SUCCESS;
}

LfsResultValue<vector<VFSDirEntry>> LevelVFS::listDir(const string& path) {
    if (!mounted) {
        return LfsResultValue<vector<VFSDirEntry>>(LFS_ERR_NOT_MOUNTED);
    }
    
    uint64_t contentCluster = context.currentContentCluster;
    
    if (!path.empty() && path != "/" && path != ".") {
        PathResult pr = resolvePath(path);
        if (!pr.valid) {
            return LfsResultValue<vector<VFSDirEntry>>(LFS_ERR_NOT_FOUND);
        }
        
        if (!pr.name.empty()) {
            FindResult found = findEntry(pr.parentCluster, pr.name);
            if (!found.found) {
                return LfsResultValue<vector<VFSDirEntry>>(LFS_ERR_NOT_FOUND);
            }
            
            if (found.entry.type != TYPE_LEVELED_DIR) {
                return LfsResultValue<vector<VFSDirEntry>>(LFS_ERR_NOT_DIR);
            }
            
            VersionEntry versions[CLUSTER_SIZE / sizeof(VersionEntry)];
            readClusterData(found.entry.startCluster, versions);
            
            for (int i = 0; i < CLUSTER_SIZE / sizeof(VersionEntry); i++) {
                if (versions[i].isActive) {
                    contentCluster = versions[i].contentTableCluster;
                    break;
                }
            }
        }
    }
    
    vector<DirEntry> entries = entryReader->readEntriesFromCluster(contentCluster);
    vector<VFSDirEntry> result;
    
    for (const DirEntry& e : entries) {
        if (e.type == TYPE_FREE) continue;
        
        VFSDirEntry de;
        char nameBuf[25];
        memcpy(nameBuf, e.name, 24);
        nameBuf[24] = '\0';
        de.name = nameBuf;
        
        if (e.extension[0] != '\0') {
            de.name += ".";
            de.name += e.extension;
        }
        
        switch (e.type) {
            case TYPE_FILE: de.type = VFS_TYPE_FILE; break;
            case TYPE_LEVELED_DIR: de.type = VFS_TYPE_DIR; break;
            case TYPE_SYMLINK: de.type = VFS_TYPE_SYMLINK; break;
            case TYPE_LEVEL_MOUNT: de.type = VFS_TYPE_LEVEL; break;
            default: de.type = VFS_TYPE_UNKNOWN; break;
        }
        
        de.size = e.size;
        de.permissions = e.attributes;
        result.push_back(de);
    }
    
    return LfsResultValue<vector<VFSDirEntry>>(result);
}

LfsResultValue<VFSStat> LevelVFS::stat(const string& path) {
    if (!mounted) {
        return LfsResultValue<VFSStat>(LFS_ERR_NOT_MOUNTED);
    }
    
    PathResult pr = resolvePath(path);
    if (!pr.valid) {
        return LfsResultValue<VFSStat>(LFS_ERR_NOT_FOUND);
    }
    
    if (pr.name.empty()) {
        VFSStat st;
        st.type = VFS_TYPE_DIR;
        st.permissions = PERM_READ | PERM_WRITE | PERM_EXEC;
        return LfsResultValue<VFSStat>(st);
    }
    
    FindResult found = findEntry(pr.parentCluster, pr.name);
    if (!found.found) {
        return LfsResultValue<VFSStat>(LFS_ERR_NOT_FOUND);
    }
    
    VFSStat st;
    switch (found.entry.type) {
        case TYPE_FILE: st.type = VFS_TYPE_FILE; break;
        case TYPE_LEVELED_DIR: st.type = VFS_TYPE_DIR; break;
        case TYPE_SYMLINK: st.type = VFS_TYPE_SYMLINK; break;
        case TYPE_LEVEL_MOUNT: st.type = VFS_TYPE_LEVEL; break;
        default: st.type = VFS_TYPE_UNKNOWN; break;
    }
    
    st.size = found.entry.size;
    st.permissions = found.entry.attributes;
    st.createTime = found.entry.createTime;
    st.modTime = found.entry.modTime;
    st.startCluster = found.entry.startCluster;
    st.levelID = 0;
    memcpy(st.name, found.entry.name, 24);
    st.name[24] = '\0';
    memcpy(st.extension, found.entry.extension, 8);
    st.extension[8] = '\0';
    
    return LfsResultValue<VFSStat>(st);
}

bool LevelVFS::exists(const string& path) {
    if (!mounted) return false;
    
    PathResult pr = resolvePath(path);
    if (!pr.valid) return false;
    
    if (pr.name.empty()) return true;
    
    FindResult found = findEntry(pr.parentCluster, pr.name);
    return found.found;
}

int LevelVFS::loadLevel(const string& levelName) {
    LFS_ENSURE_MOUNTED();
    
    LevelDescriptor levels[CLUSTER_SIZE / sizeof(LevelDescriptor)];
    if (readClusterData(sb.levelRegistryCluster, levels) != LFS_SUCCESS) {
        return LFS_ERR_DISK_IO;
    }
    
    for (int i = 0; i < CLUSTER_SIZE / sizeof(LevelDescriptor); i++) {
        if (levels[i].levelID != 0 && strcmp(levels[i].name, levelName.c_str()) == 0) {
            context.currentLevelID = levels[i].levelID;
            context.currentContentCluster = levels[i].rootContentCluster;
            return LFS_SUCCESS;
        }
    }
    
    return LFS_ERR_NOT_FOUND;
}

int LevelVFS::loadLevelByID(uint32_t levelID) {
    LFS_ENSURE_MOUNTED();
    
    LevelDescriptor levels[CLUSTER_SIZE / sizeof(LevelDescriptor)];
    if (readClusterData(sb.levelRegistryCluster, levels) != LFS_SUCCESS) {
        return LFS_ERR_DISK_IO;
    }
    
    for (int i = 0; i < CLUSTER_SIZE / sizeof(LevelDescriptor); i++) {
        if (levels[i].levelID == levelID) {
            context.currentLevelID = levels[i].levelID;
            context.currentContentCluster = levels[i].rootContentCluster;
            return LFS_SUCCESS;
        }
    }
    
    return LFS_ERR_NOT_FOUND;
}

vector<string> LevelVFS::listLevels() {
    vector<string> result;
    if (!mounted) return result;
    
    LevelDescriptor levels[CLUSTER_SIZE / sizeof(LevelDescriptor)];
    if (readClusterData(sb.levelRegistryCluster, levels) != LFS_SUCCESS) {
        return result;
    }
    
    for (int i = 0; i < CLUSTER_SIZE / sizeof(LevelDescriptor); i++) {
        if (levels[i].levelID != 0 && levels[i].name[0] != '\0') {
            result.push_back(levels[i].name);
        }
    }
    
    return result;
}

int LevelVFS::createLevel(const string& parentPath, const string& levelName) {
    LFS_ENSURE_MOUNTED();
    return LFS_ERR_GENERAL;
}

int LevelVFS::deleteLevel(const string& levelPath) {
    LFS_ENSURE_MOUNTED();
    return LFS_ERR_GENERAL;
}

int LevelVFS::chmod(const string& path, uint32_t permissions) {
    LFS_ENSURE_MOUNTED();
    
    PathResult pr = resolvePath(path);
    if (!pr.valid || pr.name.empty()) {
        return LFS_ERR_INVALID;
    }
    
    FindResult found = findEntry(pr.parentCluster, pr.name);
    if (!found.found) {
        return LFS_ERR_NOT_FOUND;
    }
    
    found.entry.attributes = permissions;
    found.entry.modTime = time(nullptr);
    
    if (!entryWriter->writeEntry(found.location.cluster, found.location.sector,
        found.location.index, found.entry)) {
        return LFS_ERR_DISK_IO;
    }
    
    return LFS_SUCCESS;
}

uint64_t LevelVFS::getFreeSpace() {
    if (!mounted) return 0;
    return sb.totalFreeClusters * CLUSTER_SIZE;
}

uint64_t LevelVFS::getTotalSpace() {
    if (!mounted) return 0;
    return sb.totalClusters * CLUSTER_SIZE;
}

uint64_t LevelVFS::getUsedSpace() {
    if (!mounted) return 0;
    return (sb.totalClusters - sb.totalFreeClusters) * CLUSTER_SIZE;
}

int LevelVFS::sync() {
    LFS_ENSURE_MOUNTED();
    writeSuperBlock();
    return LFS_SUCCESS;
}
